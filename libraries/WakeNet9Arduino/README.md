# WakeNet9Arduino 2.2.2

Quiet Arduino wrapper around Espressif ESP-SR WakeNet for the `wn9_hiesp` "Hi ESP" wake-word model on ESP32-S3.

- Default INMP441 pins: SD GPIO16, BCLK GPIO17, WS GPIO15.
- Sample rate: 16 kHz, 32-bit I²S input.
- Uses the external ESP-SR `model` partition; the model binary is not included.
- `setWakeWordCallback()` signals detection. Keep that callback short and handle application work in the main loop.
- Call `end()` before GroqAudioStream acquires the microphone I²S peripheral.

Requires compatible ESP-SR integration and the correct model partition.
