# All Libraries — Smart Patient Assistant

Central source collection for the Arduino libraries used by the Smart Patient Assistant.

## Library folders

- [WakeNet9Arduino](libraries/WakeNet9Arduino/) — ESP-SR wake-word detection for "Hi ESP".
- [GroqAudioStream](libraries/GroqAudioStream/) — INMP441 capture and Groq Whisper transcription with a recording-state callback.
- [GroqReasoning](libraries/GroqReasoning/) — AI intent interpretation and structured action responses. The planned AI-generated task summaries and pending-clarification update are still a follow-up implementation step.
- [GroqTTS](libraries/GroqTTS/) — speech output for the MAX98357A.
- [WhatsAppAPI](libraries/WhatsAppAPI/) — Evolution API message submission.
- [PatientController](libraries/PatientController/) — UART requests and acknowledgements for the secondary ESP32.
- [RGBLED](libraries/RGBLED/) — the single owner of built-in RGB status LED output.

Each library has its own `library.properties`, `src/`, and README.

## Installation

Copy the selected library folder from `libraries/` into your Arduino libraries directory, or zip that library folder and use **Sketch → Include Library → Add .ZIP Library** in Arduino IDE.

The project expects a compatible Arduino-ESP32 core 3.x. WakeNet also needs the matching ESP-SR integration and external `model` partition containing the required wake-word model.

## Status and security

This is a source collection, not a claim that the full firmware has been compiled or hardware-tested. The newly agreed GroqReasoning task-memory behaviour still needs implementation and verification.

Never commit Wi-Fi credentials, API keys, or real contact phone numbers. Keep them in the firmware's private `secrets.h`.
