# GroqAudioStream 1.3.2

ESP32 microphone capture and Groq Whisper speech-to-text with a recording-state callback.

- Uses the INMP441 and Arduino-ESP32 `ESP_I2S.h` API.
- `transcribe()` returns transcript text or an empty string on failure/no speech.
- `setRecordingStateCallback(callback)` calls back with `true` before audio capture and `false` immediately after the microphone is released.
- Use the callback to stop the blue recording LED at the microphone boundary.

This version is based on the published GroqAudioStream source and adds the recording-boundary callback required by the Smart Patient Assistant.
