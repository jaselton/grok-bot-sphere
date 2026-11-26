// Audio path for the 1.85C V2: ES7210 (2 mics) -> I2S RX; I2S TX -> ES8311 -> NS4150B amp -> speaker.
// Codec drivers: es7210.* / es8311.* from Waveshare demos 08_esp_sr / 03_audio_out_no_tf.
// Mic: ONE task (mic_task) owns I2S RX. It feeds the wake-word front end and, during a turn, a mono ring for the relay.
// Wake word: esp-sr AFE + WakeNet only ("Hi ESP"), driven directly. The Arduino ESP_SR wrapper is NOT used: it always
// builds MultiNet, which panicked in build_fsts on this core (3.3.13) and wastes RAM we need for Wi-Fi + TLS.
#include "audio.h"
#include "config.h"
#include <ESP_I2S.h>
extern "C" {
#include "esp_afe_sr_models.h"
#include "esp_afe_sr_iface.h"
#include "esp_wn_iface.h"
#include "model_path.h"
}
#include "freertos/ringbuf.h"
#include <atomic>
#include <math.h>
#include "esp_check.h"
#include "es8311.h"
#include "es7210.h"

static const char* TAG = "audio";

// ---- volume: 10 levels (ES8311 0-100 scale), saved in NVS; swipe / side button / voice / serial step it ----
#include <Preferences.h>
static const int VOLUME_LEVELS[] = { 35, 41, 47, 53, 59, 65, 71, 77, 83, 90 };   // ~1.5 dB apart; default level 6 (65)
static const int VOLUME_COUNT = sizeof(VOLUME_LEVELS) / sizeof(VOLUME_LEVELS[0]);
static int volLevel = 5;
static es8311_handle_t spk = nullptr;

// ES8311 (speaker DAC) setup, from Waveshare demo 03_audio_out_no_tf, at our 16 kHz rate.
static esp_err_t es8311_codec_init() {
  es8311_handle_t h = es8311_create(I2C_NUM_0, ES8311_ADDRRES_0);
  spk = h;
  ESP_RETURN_ON_FALSE(h, ESP_FAIL, TAG, "es8311 create failed");
  const es8311_clock_config_t clk = {
    .mclk_inverted = false, .sclk_inverted = false, .mclk_from_mclk_pin = true,
    .mclk_frequency = CODEC_RATE * 256, .sample_frequency = CODEC_RATE };
  ESP_RETURN_ON_ERROR(es8311_init(h, &clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16), TAG, "es8311 init failed");
  ESP_RETURN_ON_ERROR(es8311_voice_volume_set(h, VOLUME_LEVELS[volLevel], NULL), TAG, "es8311 volume failed");
  ESP_RETURN_ON_ERROR(es8311_microphone_config(h, false), TAG, "es8311 mic config failed");
  return ESP_OK;
}
// ES7210 (dual mic ADC) setup, from Waveshare demo 08_esp_sr.
static es7210_dev_handle_t es7210_handle = NULL;
static esp_err_t es7210_codec_init() {
  es7210_i2c_config_t i2c_conf = {}; i2c_conf.i2c_addr = 0x40;
  esp_err_t e = es7210_new_codec(&i2c_conf, &es7210_handle);
  if (e != ESP_OK) { Serial.printf("es7210_new_codec err 0x%x\n", e); return e; }
  es7210_codec_config_t c = {};
  c.i2s_format = ES7210_I2S_FMT_I2S; c.mclk_ratio = 256; c.sample_rate_hz = CODEC_RATE;
  c.bit_width = ES7210_I2S_BITS_16B; c.mic_bias = ES7210_MIC_BIAS_2V87; c.mic_gain = ES7210_MIC_GAIN_36DB;
  c.flags.tdm_enable = false;
  e = es7210_config_codec(es7210_handle, &c);
  if (e != ESP_OK) { Serial.printf("es7210_config_codec err 0x%x\n", e); return e; }
  e = es7210_config_volume(es7210_handle, 0);   // valid range -95..+32 dB (demo used 40 = out of range); gain comes from MIC_GAIN_36DB
  if (e != ESP_OK) Serial.printf("es7210_config_volume err 0x%x\n", e);
  return e;
}

static I2SClass i2s;
static RingbufHandle_t playRb;                    // mono 24 kHz PCM16 speech (from the relay) + generated chimes
static volatile uint8_t outLevel = 0;
static void (*wakeCb)() = nullptr;
static volatile bool wakePaused = true;           // nothing is fed to the wake word until wake_begin()
static RingbufHandle_t micRb;                     // mono 16 kHz mic audio for the relay (only filled during a turn)
static volatile bool micStreaming = false;
static void wake_feed(const int16_t* stereo, size_t frames);

// Audio path (after the Mosaico build, which found 16 kHz/linear-2x "super muffled"):
//   codec runs at 48 kHz stereo. Mic: 48 kHz -> 3:1 FIR decimator -> 16 kHz (wake word + relay uplink).
//   Speaker: 24 kHz from the relay -> 2x polyphase windowed-sinc upsampler -> high-pass 150 Hz -> high-shelf +6 dB
//   at 2.8 kHz -> 0.6 pre-gain -> tanh soft limiter -> 48 kHz stereo. A jitter buffer smooths network gaps.
static const size_t PLAY_RB_BYTES = 1024 * 1024;  // ~21 s of 24 kHz mono in PSRAM; the relay sends faster than real time
static volatile bool playBusy = false;
static std::atomic<int32_t> queuedBytes{0};       // bytes in the ring not yet handed to I2S
static std::atomic<uint32_t> i2sWritten{0};       // bytes handed to I2S (debug)
static volatile bool streamEnded = true;          // relay said idle: play out whatever is queued without waiting
static std::atomic<uint32_t> gapCount{0};         // underruns during a reply (debug)

// ---- FIR design (Blackman-windowed sinc, unity DC gain) ----
static void design_lowpass(float* h, int n, float fc_over_fs) {
  float sum = 0;
  for (int i = 0; i < n; i++) {
    float m = i - (n - 1) / 2.0f;
    float x = 2.0f * fc_over_fs * m;
    float sinc = (fabsf(m) < 1e-6f) ? 2.0f * fc_over_fs : sinf(M_PI * x) / (M_PI * m);
    float w = 0.42f - 0.5f * cosf(2 * M_PI * i / (n - 1)) + 0.08f * cosf(4 * M_PI * i / (n - 1));
    h[i] = sinc * w; sum += h[i];
  }
  for (int i = 0; i < n; i++) h[i] /= sum;
}
// ---- RBJ biquad ----
struct Biquad { float b0, b1, b2, a1, a2, z1 = 0, z2 = 0;
  inline float run(float x) { float y = b0 * x + z1; z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y; return y; } };
static Biquad hp150, shelf;
static void design_eq(float fs) {
  { float w0 = 2 * M_PI * 150 / fs, c = cosf(w0), al = sinf(w0) / (2 * 0.7071f), a0 = 1 + al;   // high-pass 150 Hz, Q 0.707
    hp150 = { (1 + c) / 2 / a0, -(1 + c) / a0, (1 + c) / 2 / a0, -2 * c / a0, (1 - al) / a0 }; }
  { float A = powf(10, 6.0f / 40), w0 = 2 * M_PI * 2800 / fs, c = cosf(w0), sn = sinf(w0);     // high-shelf +6 dB, 2.8 kHz, S=1
    float al = sn / 2 * sqrtf((A + 1 / A) * (1 / 1.0f - 1) + 2), sa = 2 * sqrtf(A) * al;
    float a0 = (A + 1) - (A - 1) * c + sa;
    shelf = { A * ((A + 1) + (A - 1) * c + sa) / a0, -2 * A * ((A - 1) + (A + 1) * c) / a0, A * ((A + 1) + (A - 1) * c - sa) / a0,
              2 * ((A - 1) - (A + 1) * c) / a0, ((A + 1) - (A - 1) * c - sa) / a0 }; }
}
// ---- 2x upsampler: 48-tap prototype at 48 kHz, cutoff 11 kHz, 24 taps per phase, x2 gain ----
static const int UP_TAPS = 48, UP_PH = UP_TAPS / 2;
static float upH[UP_TAPS];
static float upHist[2 * UP_PH];                   // circular history, written twice so the window is contiguous
static int upIdx = 0;
// ---- 3:1 decimator for the mic: 64 taps, cutoff 6 kHz at 48 kHz ----
static const int DN_TAPS = 64;
static float dnH[DN_TAPS];
static float dnHistL[2 * DN_TAPS], dnHistR[2 * DN_TAPS];   // circular, written twice (no per-sample memmove)
static int dnIdx = 0;

static inline int16_t soft_limit(float v) {       // linear up to 24000, tanh above (no hard clipping)
  const float knee = 24000.f, room = 32767.f - knee;
  float a = fabsf(v);
  if (a > knee) a = knee + room * tanhf((a - knee) / room);
  return (int16_t)(v < 0 ? -a : a);
}

// Jitter buffer: start a reply after 500 ms is queued (or 700 ms after the first sample, or when the relay says it
// is done); after running dry mid-reply, wait for 400 ms again. Values from the Mosaico build (300 ms was choppy).
static const int32_t START_BYTES = 24000 * 2 * 500 / 1000, REFILL_BYTES = 24000 * 2 * 400 / 1000;
static const uint32_t MAX_WAIT_MS = 700;

static void play_task(void*) {
  static int16_t in[480];                          // 20 ms at 24 kHz
  static int16_t out[480 * 2 * 2];                 // 20 ms at 48 kHz, stereo
  bool priming = true; int32_t need = START_BYTES; uint32_t primeT0 = 0;
  for (;;) {
    if (priming) {
      int32_t q = queuedBytes.load();
      if (q <= 0) { primeT0 = 0; outLevel = 0; playBusy = false; vTaskDelay(pdMS_TO_TICKS(10)); continue; }
      if (!primeT0) primeT0 = millis();
      if (q < need && !streamEnded && millis() - primeT0 < MAX_WAIT_MS) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
      priming = false; primeT0 = 0;
    }
    size_t n;
    int16_t* p = (int16_t*)xRingbufferReceiveUpTo(playRb, &n, pdMS_TO_TICKS(30), sizeof in);
    if (!p) {                                      // ran dry
      if (playBusy && !streamEnded) gapCount++;
      priming = true; need = REFILL_BYTES; playBusy = false; outLevel = 0;
      continue;
    }
    playBusy = true;
    queuedBytes -= (int32_t)n;
    size_t s = n / 2; memcpy(in, p, n); vRingbufferReturnItem(playRb, p);
    int64_t acc = 0;
    for (size_t i = 0; i < s; i++) {
      upIdx = upIdx ? upIdx - 1 : UP_PH - 1;
      upHist[upIdx] = upHist[upIdx + UP_PH] = in[i];
      const float* w = upHist + upIdx;                 // newest first, contiguous
      for (int ph = 0; ph < 2; ph++) {
        float y = 0; for (int k = 0; k < UP_PH; k++) y += upH[2 * k + ph] * w[k];
        y = shelf.run(hp150.run(y * 2.0f)) * 0.6f;
        int16_t v = soft_limit(y);
        out[(2 * i + ph) * 2] = out[(2 * i + ph) * 2 + 1] = v;
        acc += abs(v);
      }
    }
    outLevel = (uint8_t)min<int64_t>(255, (acc / (s ? 2 * s : 1)) / 64);
    i2sWritten += i2s.write((uint8_t*)out, s * 2 * 4);
  }
}
// Owns I2S RX: 20 ms at 48 kHz stereo -> 3:1 decimation -> 16 kHz stereo -> wake word (when armed) + relay ring.
static void mic_task(void*) {
  static int16_t raw[960 * 2];                     // 960 stereo frames = 20 ms at 48 kHz
  static int16_t st[320 * 2];                      // 320 stereo frames at 16 kHz
  static int16_t mono[320];
  int phase = 0; size_t outN = 0;
  for (;;) {
    size_t got = i2s.readBytes((char*)raw, sizeof raw);
    size_t n = got / 4;
    if (!n) continue;
    outN = 0;
    for (size_t i = 0; i < n; i++) {
      dnIdx = dnIdx ? dnIdx - 1 : DN_TAPS - 1;
      dnHistL[dnIdx] = dnHistL[dnIdx + DN_TAPS] = raw[2 * i];
      dnHistR[dnIdx] = dnHistR[dnIdx + DN_TAPS] = raw[2 * i + 1];
      if (++phase < 3) continue;                     // only every 3rd input produces an output
      phase = 0;
      const float* wl = dnHistL + dnIdx; const float* wr = dnHistR + dnIdx;
      float l = 0, r = 0; for (int k = 0; k < DN_TAPS; k++) { l += dnH[k] * wl[k]; r += dnH[k] * wr[k]; }
      st[2 * outN] = (int16_t)constrain(l, -32768.f, 32767.f); st[2 * outN + 1] = (int16_t)constrain(r, -32768.f, 32767.f);
      if (++outN == 320) break;
    }
    if (!outN) { vTaskDelay(1); continue; }
    if (!wakePaused) wake_feed(st, outN);
    if (micStreaming) {
      for (size_t i = 0; i < outN; i++) mono[i] = st[2 * i];   // left mic (MIC1)
      xRingbufferSend(micRb, mono, outN * 2, 0);                 // drop if the relay falls >2 s behind
    }
  }
}
static void volume_load() { Preferences p; p.begin("audio", true); volLevel = constrain((int)p.getUChar("vol10", 5), 0, VOLUME_COUNT - 1); p.end(); }   // "vol10": new key (old "vol" was 5 levels)
int audio_volume_level() { return volLevel; }
int audio_volume_levels() { return VOLUME_COUNT; }
void audio_set_volume_level(int lvl, bool chime) {
  volLevel = constrain(lvl, 0, VOLUME_COUNT - 1);
  if (spk) es8311_voice_volume_set(spk, VOLUME_LEVELS[volLevel], NULL);
  Preferences p; p.begin("audio", false); p.putUChar("vol10", volLevel); p.end();
  Serial.printf("[audio] volume level %d/%d (%d)\n", volLevel + 1, VOLUME_COUNT, VOLUME_LEVELS[volLevel]);
  if (chime) audio_chime(2);
}
void audio_init() {
  volume_load();
  design_lowpass(upH, UP_TAPS, 11000.f / CODEC_RATE);
  design_lowpass(dnH, DN_TAPS, 6000.f / CODEC_RATE);   // 6 kHz: well clear of the 8 kHz fold (7 kHz aliased hiss into 7-8 kHz)
  design_eq(CODEC_RATE);
  pinMode(PIN_PA_EN, OUTPUT); digitalWrite(PIN_PA_EN, HIGH);
  i2s.setPins(PIN_I2S_BCK, PIN_I2S_LRCK, PIN_I2S_DOUT, PIN_I2S_DIN, PIN_I2S_MCLK);
  i2s.begin(I2S_MODE_STD, CODEC_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  if (es7210_codec_init() != ESP_OK) Serial.println("ES7210 mic codec init FAILED");
  if (es8311_codec_init() != ESP_OK) Serial.println("ES8311 speaker codec init FAILED");
  playRb = xRingbufferCreateWithCaps(PLAY_RB_BYTES, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);
  xTaskCreatePinnedToCore(play_task, "play", 6144, nullptr, 5, nullptr, 0);
  micRb = xRingbufferCreateWithCaps(64 * 1024, RINGBUF_TYPE_BYTEBUF, MALLOC_CAP_SPIRAM);   // ~2 s of mono
  xTaskCreatePinnedToCore(mic_task, "mic", 6144, nullptr, 4, nullptr, 1);
}
size_t audio_read_mic(int16_t* out, size_t maxS, uint32_t timeoutMs) {
  size_t n; void* p = xRingbufferReceiveUpTo(micRb, &n, pdMS_TO_TICKS(timeoutMs), maxS * 2);
  if (!p) return 0;
  memcpy(out, p, n); vRingbufferReturnItem(micRb, p);
  return n / 2;
}
void audio_mic_stream(bool on) {
  if (on) { size_t n; void* p; while ((p = xRingbufferReceive(micRb, &n, 0))) vRingbufferReturnItem(micRb, p); }   // no stale audio
  micStreaming = on;
}
void audio_play(const int16_t* in, size_t n) {    // n mono samples at SPK_RATE (24 kHz)
  if (xRingbufferSend(playRb, in, n * 2, pdMS_TO_TICKS(20)) == pdTRUE) queuedBytes += (int32_t)(n * 2);
  else Serial.println("playback buffer full, audio dropped");
}
void audio_stream_begin() { streamEnded = false; gapCount = 0; }
void audio_stream_end()   { streamEnded = true; if (gapCount) Serial.printf("[audio] reply had %lu playback gaps\n", (unsigned long)gapCount.load()); }
void audio_flush_playback() { size_t n; void* p; while ((p = xRingbufferReceive(playRb, &n, 0))) vRingbufferReturnItem(playRb, p); queuedBytes = 0; streamEnded = true; }
bool audio_playing() { return playBusy || queuedBytes.load() > 0; }
uint8_t audio_out_level() { return outLevel; }

// Chimes, generated at 24 kHz into the playback ring (so they get the same upsampling/EQ).
static void tone(float f, int ms, float amp, int16_t* buf, int& pos) {
  int n = SPK_RATE * ms / 1000;
  for (int i = 0; i < n; i++) {
    float env = expf(-4.0f * i / n) * (i < 120 ? i / 120.f : 1.f);        // soft attack, bell-like decay
    buf[pos++] = (int16_t)(amp * env * (sinf(2 * M_PI * f * i / SPK_RATE) + 0.25f * sinf(4 * M_PI * f * i / SPK_RATE)));
  }
}
void audio_chime(int kind) {                      // 0 = result ready (two rising notes), 1 = alarm (three notes)
  static int16_t* buf = (int16_t*)heap_caps_malloc(SPK_RATE * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM);   // PSRAM: internal RAM is for DMA/Wi-Fi
  if (!buf) return;
  int pos = 0;
  if (kind == 1) { tone(1318.5f, 180, 9000, buf, pos); tone(1568.0f, 180, 9000, buf, pos); tone(2093.0f, 320, 9000, buf, pos); }
  else if (kind == 2) { tone(1760.0f, 140, 8000, buf, pos); }                 // volume tick
  else           { tone(1046.5f, 160, 7000, buf, pos); tone(1568.0f, 300, 7000, buf, pos); }
  for (int i = 0; i < SPK_RATE / 8; i++) buf[pos++] = 0;                  // short tail of silence
  streamEnded = true;                                                    // play immediately, no jitter wait
  audio_play(buf, pos);
}

#if __has_include("wake_model.h")
// ---------------- wake word: custom microWakeWord model ("Hey Charlie") via TFLite Micro (wake_tflm.cpp) ----------------
bool tflm_wake_begin(void (*cb)());
void tflm_wake_feed(const int16_t* mono, size_t n);
void tflm_wake_set_enabled(bool on);
void tflm_wake_set_cutoff(float c);
void tflm_wake_stats(float& last, float& peak, float& cutoff);
void tflm_wake_test_mode(bool on);
static bool tflmReady = false;
static void tflm_fired() { wakePaused = true; if (wakeCb) wakeCb(); }
static void wake_feed(const int16_t* stereo, size_t frames) {    // called from mic_task: left mic, 16 kHz mono
  static int16_t mono[320];
  if (!tflmReady) return;
  size_t n = frames > 320 ? 320 : frames;
  for (size_t i = 0; i < n; i++) mono[i] = stereo[2 * i];
  tflm_wake_feed(mono, n);
}
bool wake_begin(void (*cb)()) {
  wakeCb = cb;
  tflmReady = tflm_wake_begin(tflm_fired);
  if (tflmReady) wakePaused = false;
  return tflmReady;
}
void wake_pause()  { wakePaused = true; if (tflmReady) tflm_wake_set_enabled(false); }
void wake_resume() { if (tflmReady) { tflm_wake_set_enabled(true); wakePaused = false; } }
#else
// ---------------- wake word: esp-sr AFE + WakeNet ("Hi ESP") ----------------
static const esp_afe_sr_iface_t* afe = nullptr;
static esp_afe_sr_data_t* afeData = nullptr;
static int16_t* feedBuf = nullptr;
static int feedChunk = 0, feedCh = 0, feedFill = 0;

static void wake_feed(const int16_t* stereo, size_t frames) {    // called from mic_task
  if (!afeData) return;
  for (size_t i = 0; i < frames; i++) {
    for (int c = 0; c < feedCh; c++) feedBuf[feedFill * feedCh + c] = stereo[2*i + (c < 2 ? c : 0)];
    if (++feedFill == feedChunk) { afe->feed(afeData, feedBuf); feedFill = 0; }
  }
}
static void wake_detect_task(void*) {
  for (;;) {
    afe_fetch_result_t* r = afe->fetch_with_delay(afeData, pdMS_TO_TICKS(500));
    if (!r || r->ret_value == ESP_FAIL) continue;
    if (r->wakeup_state == WAKENET_DETECTED && !wakePaused) {
      Serial.println("[wake] Hi ESP detected");
      wakePaused = true;                           // the app resumes us when the turn is over
      if (wakeCb) wakeCb();
    }
  }
}
bool wake_begin(void (*cb)()) {
  wakeCb = cb;
  srmodel_list_t* models = esp_srmodel_init("model");             // the "model" partition (srmodels.bin)
  if (!models) { Serial.println("[wake] no model partition"); return false; }
  afe_config_t* cfg = afe_config_init("MM", models, AFE_TYPE_SR, AFE_MODE_LOW_COST);   // 2 mics, no playback reference
  if (!cfg) { Serial.println("[wake] afe_config_init failed"); return false; }
  cfg->aec_init = false; cfg->se_init = false; cfg->ns_init = false; cfg->vad_init = false; cfg->agc_init = false;
  cfg->wakenet_init = true;
  cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;           // keep internal RAM for Wi-Fi + TLS
  cfg->afe_perferred_core = 1;
  Serial.printf("[wake] model %s\n", cfg->wakenet_model_name ? cfg->wakenet_model_name : "(none)");
  afe = esp_afe_handle_from_config(cfg);
  afeData = afe ? afe->create_from_config(cfg) : nullptr;
  afe_config_free(cfg);
  if (!afeData) { Serial.println("[wake] AFE create failed"); return false; }
  feedChunk = afe->get_feed_chunksize(afeData);
  feedCh = afe->get_feed_channel_num(afeData);
  feedBuf = (int16_t*)heap_caps_malloc(feedChunk * feedCh * sizeof(int16_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!feedBuf) { Serial.println("[wake] feed buffer alloc failed"); return false; }
  xTaskCreatePinnedToCore(wake_detect_task, "wake", 6144, nullptr, 5, nullptr, 1);
  Serial.printf("[wake] ready: chunk %d x %d ch\n", feedChunk, feedCh);
  wakePaused = false;
  return true;
}
void wake_pause()  { wakePaused = true; feedFill = 0; }
void wake_resume() { if (afeData) { afe->reset_buffer(afeData); wakePaused = false; } }

#endif

// ---- USB debug: TONE (1 s, 1 kHz through the normal playback path), MIC (2 s level), AUDIO? (counters) ----
#include <math.h>
void audio_debug_command(const String& c) {
  if (c == "TONE") {
    static int16_t* buf = (int16_t*)heap_caps_malloc(SPK_RATE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!buf) return;
    for (int i = 0; i < SPK_RATE; i++) buf[i] = (int16_t)(8000 * sinf(2 * M_PI * 1000 * i / (float)SPK_RATE));
    streamEnded = true; uint32_t w0 = i2sWritten; audio_play(buf, SPK_RATE);
    uint32_t t0 = millis(); while (audio_playing() && millis() - t0 < 3000) delay(10);
    Serial.printf("[audio] TONE done in %lu ms, i2s wrote %lu bytes, queued now %ld, PA_EN=%d\n", millis() - t0,
                  (unsigned long)(i2sWritten - w0), (long)queuedBytes.load(), digitalRead(PIN_PA_EN));
  } else if (c == "MIC") {
    static int16_t m[320]; audio_mic_stream(true); uint32_t t0 = millis(); size_t total = 0; int64_t acc = 0; int peak = 0;
    while (millis() - t0 < 2000) { size_t n = audio_read_mic(m, 320, 50); total += n;
      for (size_t i = 0; i < n; i++) { acc += abs(m[i]); peak = max(peak, (int)abs(m[i])); } }
    audio_mic_stream(false);
    Serial.printf("[audio] MIC 2 s: %u samples (expect ~32000), mean |x| %ld, peak %d\n", (unsigned)total, (long)(total ? acc / (int64_t)total : 0), peak);
  } else if (c == "MICDUMP") {                       // 3 s of the 16 kHz uplink as hex lines (for analysis on the Mac)
    static int16_t m[320]; audio_mic_stream(true); uint32_t t0 = millis(); size_t total = 0;
    Serial.println("[dump] begin 16000");
    while (millis() - t0 < 3000) {
      size_t n = audio_read_mic(m, 320, 50); total += n;
      if (n) { Serial.print("[dump] "); for (size_t i = 0; i < n; i++) Serial.printf("%04x", (uint16_t)m[i]); Serial.println(); }
    }
    audio_mic_stream(false);
    Serial.printf("[dump] end %u\n", (unsigned)total);
#if __has_include("wake_model.h")
  } else if (c.startsWith("CUTOFF ")) { tflm_wake_set_cutoff(c.substring(7).toFloat());
  } else if (c == "WAKETEST ON") { tflm_wake_test_mode(true);  wakePaused = false;
  } else if (c == "WAKETEST OFF") { tflm_wake_test_mode(false);
  } else if (c == "WAKE?") { float l, p, cu; tflm_wake_stats(l, p, cu); Serial.printf("[wake] last %.2f peak %.2f cutoff %.2f\n", l, p, cu);
#endif
  } else if (c.startsWith("VOL ")) { audio_set_volume_level(c.substring(4).toInt() - 1, true);
  } else if (c == "CHIME") { audio_chime(0);
  } else if (c == "AUDIO?") {
    Serial.printf("[audio] i2s wrote %lu, queued %ld, busy %d, PA_EN=%d\n", (unsigned long)i2sWritten.load(), (long)queuedBytes.load(), (int)playBusy, digitalRead(PIN_PA_EN));
  }
}
