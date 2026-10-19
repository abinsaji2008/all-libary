#ifndef GROQ_TTS_H
#define GROQ_TTS_H

#include <Arduino.h>

/*
 * GroqTTS for ESP32-S3 + MAX98357A
 *
 * Minimal public API:
 *   speaker.begin(apiKey);
 *   speaker.say("Hello");
 *
 * Defaults:
 *   BCLK = GPIO 7, WS/LRC = GPIO 8, DIN = GPIO 9
 *   Model = canopylabs/orpheus-v1-english
 *   Voice = troy
 *
 * Requires Arduino-ESP32 core 3.x. The WAV output is buffered before
 * playback so the speaker receives each generated response only once.
 */
class GroqTTS {
public:
  GroqTTS();

  // Call once after Wi-Fi has connected.
  bool begin(const char *apiKey);
  bool begin(const String &apiKey) { return begin(apiKey.c_str()); }

  // Optional overrides. Defaults are BCLK=7, WS=8, DIN=9.
  void setPins(int8_t bclkPin, int8_t wsPin, int8_t dataPin);
  void setVoice(const char *voice);
  void setModel(const char *model);

  // Digital attenuation: 0=mute, 100=full-scale. Default is 50.
  void setVolume(uint8_t percent);

  // Generate and play text. Long text is split into API-sized chunks.
  bool say(const String &text);
  bool say(const char *text) { return text ? say(String(text)) : false; }

  String lastError() const { return lastError_; }
  int lastHttpStatus() const { return httpStatus_; }

private:
  String apiKey_;
  String model_;
  String voice_;
  String lastError_;
  int httpStatus_;
  int8_t bclkPin_;
  int8_t wsPin_;
  int8_t dataPin_;
  uint8_t volumePercent_;
  bool ready_;

  bool requestWav(const String &text, uint8_t **wav, size_t *wavLength);
  bool playWav(const uint8_t *wav, size_t wavLength);
  void fail(const String &message);
};

#endif