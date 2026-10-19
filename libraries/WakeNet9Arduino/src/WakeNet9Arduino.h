#pragma once

#include <Arduino.h>
#include <ESP_I2S.h>

// Quiet WakeNet9 wrapper for ESP32-S3 + wn9_hiesp.
// This build does not read commands from or write logs to Serial Monitor.
class WakeNet9Arduino {
public:
  struct Config {
    int bclkPin = 17;
    int wsPin = 15;
    int dataPin = 16;
    int sampleRate = 16000;
    bool use32BitInput = true;
    bool useLeftChannel = true;
    uint32_t i2sTimeoutMs = 100;
    const char *wakeWordLabel = "Hi ESP";
  };

  using WakeWordCallback = void (*)();

  WakeNet9Arduino();
  ~WakeNet9Arduino();
  bool begin(const Config &config);
  bool end();
  void update();
  void handleSerial() { update(); }
  void setSerialOutput(bool enabled);
  void setWakeWordCallback(WakeWordCallback callback);

private:
  bool beginAudio();
  I2SClass i2s_;
  void *models_;
  void *wakeData_;
  const void *wakeInterface_;
  char *wakeModelName_;
  int chunkSize_;
  bool active_;
  bool i2sStarted_;
  int sampleRate_;
  int bclkPin_;
  int wsPin_;
  int dataPin_;
  bool useLeftChannel_;
  uint32_t i2sTimeoutMs_;
  int16_t audioBuffer_[1024];
  WakeWordCallback callback_;
};
