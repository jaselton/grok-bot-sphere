# Building the Sphere firmware

FQBN: `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=esp_sr_16,PSRAM=opi`

```sh
./make_secrets.sh                                    # RELAY_URL=wss://<your relay>/ws ./make_secrets.sh  (git-ignored)
./make_wake_model.sh ~/dev/sphere-185/wakeword/hey_charlie.tflite "hey charlie" 0.80   # git-ignored; omit for esp-sr "Hi ESP"
S=$PWD; arduino-cli compile -b <FQBN> \
  --build-property "compiler.cpp.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$S -I$S/src/microfrontend -I$S/kissfft_inc" \
  --build-property "compiler.c.extra_flags=-DLV_CONF_INCLUDE_SIMPLE -I$S -I$S/src/microfrontend -I$S/kissfft_inc" \
  --output-dir ../build $S
esptool --port /dev/cu.usbmodemXXXX -b 921600 write-flash 0x10000 ../build/grok_sphere.ino.bin
```

Libraries: lvgl 9.3.0, ArduinoJson 7, WebSockets 2.7, ESP_TF 2.1.2 (TFLite Micro + ESP-NN).
**ESP_TF needs `dot_a_linkage=false`** in its library.properties (local edit): as an archive, members collide by
basename (two `common.cc`, two `kernel_util.cc`) and the link fails with undefined `TfLiteTypeGetName` etc.
`build_opt.h` turns on the ESP-NN S3 kernels (`-DESP_NN -DCONFIG_NN_OPTIMIZED -DARCH_ESP32_S3`).
The wake-word model (`wake_model.h`) must never be committed: its training data is non-commercial.

Serial debug commands: TALK, TAP, RING, TONE, CHIME, MIC, MICDUMP, AUDIO?, WIFI?, WIFI-FORGET, WAKE?, CUTOFF x,
WAKETEST ON|OFF, TIMER secs, FACE n (0 eyes 1 dive 2 timer 3 talk 4 stinger), TALKLEFT min, SHOT (canvas dump; see tools/face_shots.py). Speaker test: `~/dev/sphere-185/tools/wake_speaker_test.py "hey charlie"`.
