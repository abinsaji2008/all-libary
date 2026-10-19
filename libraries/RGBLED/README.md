# RGBLED 1.0.0

Single-owner built-in RGB status indicator for compatible ESP32 Arduino boards.

- `RGBLED::State::OFF`: off while waiting/idle.
- `RGBLED::State::RECORDING_BLUE_BLINK`: blue blinks while microphone recording is active.
- `RGBLED::State::PROCESSING_GREEN`: solid green for reasoning, action execution, and speech playback.

The application requests states using `setState()`; this library owns `rgbLedWrite()` and the non-blocking blink task. It uses the board variant's `RGB_BUILTIN` instead of hardcoding a physical GPIO. Requires Arduino-ESP32 core 3.x or later with `rgbLedWrite()` support.
