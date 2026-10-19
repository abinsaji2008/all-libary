# GroqTTS 1.0.0

Groq text-to-speech playback for ESP32-S3 with MAX98357A I²S output.

- Configure API key with `begin()`.
- Optional overrides: `setPins()`, `setModel()`, `setVoice()`, and `setVolume()`.
- Play a spoken response with `say()`.
- Default speaker pins: BCLK GPIO7, WS GPIO8, DIN GPIO9.
- The project configuration uses `canopylabs/orpheus-v1-english` and voice `troy`.

Requires Arduino-ESP32 core 3.x. This source is the current library baseline; acknowledgement/error beep helpers will need to be added and verified as part of the final project integration.
