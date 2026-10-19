#ifndef GROQ_AUDIO_STREAM_H
#define GROQ_AUDIO_STREAM_H

#define GROQ_AUDIO_STREAM_HAS_RECORDING_CALLBACK 1

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP_I2S.h>

/*
 * GroqAudioStream v1.3.1
 * One-call API:
 *   groqAudio.setConfig(config);      // optional overrides
 *   groqAudio.begin(url, apiKey);     // once in setup()
 *   String transcript = groqAudio.transcribe();
 *
 * The library captures INMP441 audio, applies high-pass filter/gain and VAD,
 * streams the audio upload to Groq Whisper, and returns only the transcript.
 * Keep LLM reasoning and final-answer generation in the main sketch.
 */
class GroqAudioStream {
public:
  struct Config {
    int sdPin = 16;
    int sckPin = 17;
    int wsPin = 15;
    bool useLeftChannel = true;
    uint32_t sampleRate = 16000;

    uint32_t maxRecordMs = 10000;
    uint32_t waitForFirstVoiceMs = 4000;
    uint32_t silenceToStopMs = 2000;
    uint32_t micReadTimeoutMs = 250;
    uint32_t groqTimeoutMs = 30000;
    uint32_t vadThreshold = 700;
    float audioGain = 8.0f;
    float highPassHz = 120.0f;
    bool debugOutput = false; // false = silent library; true = print capture/errors.
  };

  GroqAudioStream();
  void setConfig(const Config &config);

  using RecordingStateCallback = void (*)(bool recording);
  void setRecordingStateCallback(RecordingStateCallback callback);

  bool begin(const char *endpointUrl,
             const char *apiKey,
             const char *model = "whisper-large-v3-turbo",
             const char *language = "en");

  // Called after Wi-Fi is checked and WakeNet's I2S has been released.
  // Returns transcript text, or an empty String for failure/no speech.
  String transcribe();
  String lastError() const { return lastError_; }
  int lastHttpStatus() const { return httpStatus_; }

private:
  RecordingStateCallback recordingStateCallback_ = nullptr;
  WiFiClientSecure client_;
  I2SClass mic_;
  Config config_;
  String host_;
  String path_;
  String apiKey_;
  String model_;
  String language_;
  String boundary_;
  String lastError_;
  uint16_t port_;
  int httpStatus_;
  bool configured_;
  bool streamActive_;
  float hpA_ = 0.0f;
  float hpX1_ = 0.0f, hpY1_ = 0.0f, hpX2_ = 0.0f, hpY2_ = 0.0f;

  bool parseEndpoint(const char *endpointUrl);
  bool beginMicrophone();
  void endMicrophone();
  bool startUpload();
  bool sendAudioChunk(const uint8_t *data, size_t length);
  bool sendTextChunk(const String &text);
  bool sendFormField(const char *name, const String &value);
  bool finishUpload(String &transcript);
  void abortUpload();
  void makeWavHeader(uint8_t header[44]) const;
  void resetFilter();
  uint32_t filterAudio(int16_t *samples, size_t count);
  bool readLine(String &line, uint32_t timeoutMs);
  bool readResponse(String &body);
  void fail(const String &message);
};

#endif
