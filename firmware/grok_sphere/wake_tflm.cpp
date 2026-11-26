// SPDX-License-Identifier: Apache-2.0
// On-device custom wake word ("Hey Charlie") with a microWakeWord streaming model (TFLite Micro via ESP_TF).
// Ported from the ESP-Mosaico grok buddy (components/app_wake/app_wake.cc): features come from the TFLite Micro audio
// frontend configured exactly like pymicro-features (the training feature generator): 30 ms windows every 10 ms,
// 40 mel channels 125-7500 Hz, noise reduction, PCAN, log scale. Every `stride` feature frames the quantized
// streaming model returns a probability; the mean of the last kWindow scores >= cutoff fires once, then a refractory.
// Only built when wake_model.h exists (make_wake_model.sh); otherwise audio.cpp uses esp-sr WakeNet ("Hi ESP").
#if __has_include("wake_model.h")
#include "wake_model.h"
#include <Arduino.h>
#include <ESP_TF.h>
#include <atomic>
#include <math.h>
#include "esp_heap_caps.h"
#include "freertos/stream_buffer.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

namespace {
constexpr int kSampleRate = 16000;
constexpr int kFeatures = 40;
constexpr float kFeatureScale = 0.0390625f;     // pymicro-features FLOAT32_SCALE
constexpr size_t kArenaBytes = 96 * 1024;
constexpr size_t kVarArenaBytes = 2 * 1024;
constexpr int kWindow = 5;                      // scores averaged
constexpr int kRefractoryMs = 2000;
constexpr size_t kRingBytes = kSampleRate * 2;  // 1 s of PCM16

struct Wake {
  void (*cb)() = nullptr;
  StreamBufferHandle_t ring = nullptr;
  std::atomic<bool> enabled{false};
  std::atomic<bool> resumed{false};
  FrontendState frontend{};
  tflite::MicroInterpreter* interp = nullptr;
  int stride = 3;
  float in_scale = 1.0f;
  int in_zero = 0;
  float cutoff = WAKE_MODEL_CUTOFF;
  std::atomic<float> lastMean{0};
  std::atomic<float> peakMean{0};
  std::atomic<bool> testMode{false};   // never fire; log each event's peak (mean falls below peak/2 = event over)
};
Wake w;

bool frontend_init() {
  FrontendConfig c;
  FrontendFillConfigWithDefaults(&c);
  c.window.size_ms = 30; c.window.step_size_ms = 10;
  c.filterbank.num_channels = kFeatures; c.filterbank.lower_band_limit = 125.0; c.filterbank.upper_band_limit = 7500.0;
  c.noise_reduction.smoothing_bits = 10; c.noise_reduction.even_smoothing = 0.025; c.noise_reduction.odd_smoothing = 0.06;
  c.noise_reduction.min_signal_remaining = 0.05;
  c.pcan_gain_control.enable_pcan = 1; c.pcan_gain_control.strength = 0.95; c.pcan_gain_control.offset = 80.0;
  c.pcan_gain_control.gain_bits = 21;
  c.log_scale.enable_log = 1; c.log_scale.scale_shift = 6;
  return FrontendPopulateState(&c, &w.frontend, kSampleRate) != 0;
}

bool model_init() {
  const tflite::Model* model = tflite::GetModel(g_wake_model);
  if (model->version() != TFLITE_SCHEMA_VERSION) { Serial.printf("[wake] model schema %lu != %d\n", (unsigned long)model->version(), TFLITE_SCHEMA_VERSION); return false; }
  static tflite::MicroMutableOpResolver<20> ops;      // ops used by microWakeWord streaming models
  ops.AddCallOnce(); ops.AddVarHandle(); ops.AddReshape(); ops.AddReadVariable(); ops.AddStridedSlice();
  ops.AddConcatenation(); ops.AddAssignVariable(); ops.AddConv2D(); ops.AddMul(); ops.AddAdd(); ops.AddMean();
  ops.AddFullyConnected(); ops.AddLogistic(); ops.AddQuantize(); ops.AddDepthwiseConv2D(); ops.AddAveragePool2D();
  ops.AddMaxPool2D(); ops.AddPad(); ops.AddPack(); ops.AddSplitV();
  uint8_t* arena = (uint8_t*)heap_caps_aligned_alloc(16, kArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  uint8_t* var_arena = (uint8_t*)heap_caps_aligned_alloc(16, kVarArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!arena || !var_arena) return false;
  tflite::MicroAllocator* va = tflite::MicroAllocator::Create(var_arena, kVarArenaBytes);
  tflite::MicroResourceVariables* vars = tflite::MicroResourceVariables::Create(va, 20);
  static tflite::MicroInterpreter interp(model, ops, arena, kArenaBytes, vars);
  if (interp.AllocateTensors() != kTfLiteOk) { Serial.printf("[wake] AllocateTensors failed (arena %u)\n", (unsigned)kArenaBytes); return false; }
  TfLiteTensor* in = interp.input(0);
  if (in->type != kTfLiteInt8 || in->dims->size != 3 || in->dims->data[2] != kFeatures) { Serial.println("[wake] unexpected model input"); return false; }
  w.stride = in->dims->data[1]; w.in_scale = in->params.scale; w.in_zero = in->params.zero_point; w.interp = &interp;
  Serial.printf("[wake] \"%s\" model %u bytes, stride %d, arena used %u, cutoff %.2f\n", WAKE_MODEL_NAME,
                (unsigned)sizeof(g_wake_model), w.stride, (unsigned)interp.arena_used_bytes(), w.cutoff);
  return true;
}

inline int8_t quantize(uint16_t v) {
  long q = lroundf((float)v * kFeatureScale / w.in_scale) + w.in_zero;
  return (int8_t)(q < -128 ? -128 : (q > 127 ? 127 : q));
}

void wake_task(void*) {
  int16_t pcm[160];
  float scores[kWindow] = {0};
  int pos = 0, frames = 0, ignore = kWindow * 4;
  uint32_t refractoryUntil = 0;
  TfLiteTensor* in = w.interp->input(0);
  for (;;) {
    size_t got = xStreamBufferReceive(w.ring, pcm, sizeof pcm, portMAX_DELAY);
    size_t n = got / sizeof(int16_t), off = 0;
    while (off < n) {
      size_t read = 0;
      FrontendOutput o = FrontendProcessSamples(&w.frontend, pcm + off, n - off, &read);
      off += read;
      if (!o.size) continue;
      int8_t* dst = in->data.int8 + frames * kFeatures;
      for (int i = 0; i < kFeatures; ++i) dst[i] = quantize(o.values[i]);
      if (++frames < w.stride) continue;
      frames = 0;
      if (w.interp->Invoke() != kTfLiteOk) continue;
      if (w.resumed.exchange(false)) { ignore = kWindow * 4; memset(scores, 0, sizeof scores); }
      scores[pos] = (float)w.interp->output(0)->data.uint8[0] / 255.0f;
      pos = (pos + 1) % kWindow;
      if (ignore > 0) { --ignore; continue; }        // let the model state settle after a pause
      float mean = 0; for (float s : scores) mean += s; mean /= kWindow;
      w.lastMean = mean; if (mean > w.peakMean) w.peakMean = mean;
      if (w.testMode.load()) {                       // speaker test: report peaks, not threshold crossings
        static float evPeak = 0;
        if (mean > evPeak) evPeak = mean;
        if (evPeak >= 0.10f && mean < evPeak * 0.5f) { Serial.printf("[wake] event peak %.3f\n", evPeak); evPeak = 0; }
        continue;
      }
      if (mean >= w.cutoff && millis() >= refractoryUntil && w.enabled.load()) {
        Serial.printf("[wake] %s detected (score %.2f)\n", WAKE_MODEL_NAME, mean);
        refractoryUntil = millis() + kRefractoryMs;
        memset(scores, 0, sizeof scores);
        w.enabled = false;                           // the app resumes us when the turn is over
        if (w.cb) w.cb();
      }
    }
  }
}
}  // namespace

bool tflm_wake_begin(void (*cb)()) {
  w.cb = cb;
  if (!model_init() || !frontend_init()) return false;
  uint8_t* storage = (uint8_t*)heap_caps_malloc(kRingBytes + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  auto* meta = (StaticStreamBuffer_t*)heap_caps_malloc(sizeof(StaticStreamBuffer_t), MALLOC_CAP_INTERNAL);
  if (!storage || !meta) return false;
  w.ring = xStreamBufferCreateStatic(kRingBytes + 1, sizeof(int16_t), storage, meta);
  if (xTaskCreatePinnedToCore(wake_task, "wake", 8192, nullptr, 3, nullptr, 0) != pdPASS) return false;
  w.enabled = true;
  return true;
}
void tflm_wake_feed(const int16_t* mono, size_t n) { if (w.ring && w.enabled.load()) xStreamBufferSend(w.ring, mono, n * sizeof(int16_t), 0); }
void tflm_wake_set_enabled(bool on) { if (on && !w.enabled.exchange(true)) w.resumed = true; else if (!on) w.enabled = false; }
void tflm_wake_set_cutoff(float c) { w.cutoff = c; Serial.printf("[wake] cutoff %.2f\n", c); }
void tflm_wake_stats(float& last, float& peak, float& cutoff) { last = w.lastMean; peak = w.peakMean; cutoff = w.cutoff; w.peakMean = 0; }
const char* tflm_wake_name() { return WAKE_MODEL_NAME; }
void tflm_wake_test_mode(bool on) { w.testMode = on; w.enabled = true; Serial.printf("[wake] test mode %d\n", on); }
#endif
