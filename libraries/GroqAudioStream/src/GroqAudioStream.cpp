#include "GroqAudioStream.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

namespace {
static const size_t AUDIO_BUFFER_BYTES = 1024;
static const size_t MAX_HTTP_RESPONSE_BYTES = 8192;
}

GroqAudioStream::GroqAudioStream()
    : port_(443), httpStatus_(0), configured_(false), streamActive_(false) {
  boundary_ = "----esp32-groq-audio";
}

void GroqAudioStream::setConfig(const Config &config) {
  config_ = config;
}

void GroqAudioStream::setRecordingStateCallback(RecordingStateCallback callback) {
  recordingStateCallback_ = callback;
}

bool GroqAudioStream::begin(const char *endpointUrl,
                            const char *apiKey,
                            const char *model,
                            const char *language) {
  configured_ = false;
  lastError_ = "";

  if (!endpointUrl || !endpointUrl[0] || !apiKey || !apiKey[0] ||
      !model || !model[0] || !language) {
    fail("Missing endpoint URL, API key, model, or language");
    return false;
  }
  if (config_.sampleRate != 16000) {
    fail("This configuration expects 16000 Hz for the selected Whisper WAV format");
    return false;
  }
  if (config_.maxRecordMs == 0 || config_.micReadTimeoutMs == 0 ||
      config_.groqTimeoutMs == 0 || config_.audioGain < 0.1f) {
    fail("Invalid recording or network configuration");
    return false;
  }

  apiKey_ = apiKey;
  model_ = model;
  language_ = language;
  client_.setTimeout(config_.groqTimeoutMs);
  configured_ = parseEndpoint(endpointUrl);
  return configured_;
}

String GroqAudioStream::transcribe() {
  if (!configured_) {
    fail("Call begin(endpoint, apiKey) successfully before transcribe()");
    return String();
  }
  if (WiFi.status() != WL_CONNECTED) {
    fail("Wi-Fi is disconnected; transcription was not started");
    return String();
  }

  lastError_ = "";
  httpStatus_ = 0;
  if (!beginMicrophone()) return String();
  if (!startUpload()) {
    endMicrophone();
    abortUpload();
    return String();
  }

  resetFilter();
  alignas(4) uint8_t raw[AUDIO_BUFFER_BYTES];
  const uint32_t recordStarted = millis();
  uint32_t lastVoiceMs = recordStarted;
  bool speechDetected = false;
  bool uploadOk = true;

  if (recordingStateCallback_) recordingStateCallback_(true);
  if (config_.debugOutput) Serial.println("[GroqAudioStream] Speak now...");
  while ((uint32_t)(millis() - recordStarted) < config_.maxRecordMs) {
    size_t bytesRead = mic_.readBytes(reinterpret_cast<char *>(raw), sizeof(raw));
    if (bytesRead < 2) {
      if ((uint32_t)(millis() - recordStarted) >= config_.maxRecordMs) break;
      continue;
    }
    bytesRead &= ~static_cast<size_t>(1);
    const uint32_t peak = filterAudio(reinterpret_cast<int16_t *>(raw), bytesRead / 2);
    const uint32_t now = millis();

    if (peak > config_.vadThreshold) {
      speechDetected = true;
      lastVoiceMs = now;
    }

    if (!sendAudioChunk(raw, bytesRead)) {
      fail("Audio upload failed");
      uploadOk = false;
      break;
    }

    if (!speechDetected && (uint32_t)(now - recordStarted) >= config_.waitForFirstVoiceMs) {
      if (config_.debugOutput) Serial.println("[GroqAudioStream] No voice detected");
      break;
    }
    if (speechDetected && (uint32_t)(now - lastVoiceMs) >= config_.silenceToStopMs) {
      if (config_.debugOutput) Serial.println("[GroqAudioStream] Silence detected; stopping recording");
      break;
    }
  }

  endMicrophone();
  if (recordingStateCallback_) recordingStateCallback_(false);
  if (!uploadOk || !speechDetected) {
    abortUpload();
    if (!speechDetected && lastError_.length() == 0) fail("No speech detected");
    return String();
  }

  String transcript;
  if (!finishUpload(transcript)) return String();
  transcript.trim();
  if (!transcript.length()) {
    fail("Groq returned an empty transcription");
    return String();
  }
  return transcript;
}

bool GroqAudioStream::beginMicrophone() {
  mic_.setTimeout(config_.micReadTimeoutMs);
  mic_.setPins(config_.sckPin, config_.wsPin, -1, config_.sdPin, -1);

  if (!mic_.begin(I2S_MODE_STD, config_.sampleRate,
                  I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO,
                  config_.useLeftChannel ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT)) {
    fail("I2S microphone initialization failed; check INMP441 pins");
    mic_.end();
    return false;
  }
  if (!mic_.configureRX(config_.sampleRate, I2S_DATA_BIT_WIDTH_32BIT,
                        I2S_SLOT_MODE_MONO, I2S_RX_TRANSFORM_32_TO_16)) {
    fail("I2S microphone RX configuration failed");
    mic_.end();
    return false;
  }
  return true;
}

void GroqAudioStream::endMicrophone() { mic_.end(); }

bool GroqAudioStream::parseEndpoint(const char *endpointUrl) {
  String url(endpointUrl);
  url.trim();
  if (!url.startsWith("https://")) {
    fail("Endpoint URL must start with https://");
    return false;
  }
  String rest = url.substring(8);
  int pathStart = rest.indexOf('/');
  String authority;
  if (pathStart < 0) {
    authority = rest;
    path_ = "/";
  } else {
    authority = rest.substring(0, pathStart);
    path_ = rest.substring(pathStart);
  }
  if (!authority.length() || authority.indexOf('@') >= 0 || path_.indexOf('#') >= 0) {
    fail("Invalid endpoint URL");
    return false;
  }
  int colon = authority.lastIndexOf(':');
  if (colon > 0) {
    host_ = authority.substring(0, colon);
    long parsedPort = authority.substring(colon + 1).toInt();
    if (parsedPort < 1 || parsedPort > 65535) {
      fail("Invalid endpoint port");
      return false;
    }
    port_ = static_cast<uint16_t>(parsedPort);
  } else {
    host_ = authority;
    port_ = 443;
  }
  return host_.length() > 0 && path_.length() > 0;
}

bool GroqAudioStream::startUpload() {
  if (streamActive_) abortUpload();
  // Compatibility default: TLS is encrypted but server certificate identity
  // is not verified. See README security note.
  client_.setInsecure();
  client_.setTimeout(config_.groqTimeoutMs);
  if (!client_.connect(host_.c_str(), port_)) {
    fail("Could not connect to Groq transcription endpoint");
    client_.stop();
    return false;
  }

  String request;
  request.reserve(300 + host_.length() + path_.length() + apiKey_.length());
  request += "POST "; request += path_; request += " HTTP/1.1\r\nHost: "; request += host_;
  request += "\r\nAuthorization: Bearer "; request += apiKey_;
  request += "\r\nUser-Agent: Abi-GroqAudioStream\r\nContent-Type: multipart/form-data; boundary=";
  request += boundary_; request += "\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n";
  if (client_.print(request) != request.length()) {
    fail("Could not send HTTP request headers");
    client_.stop();
    return false;
  }
  streamActive_ = true;
  if (!sendFormField("model", model_) ||
      (language_.length() && !sendFormField("language", language_)) ||
      !sendFormField("response_format", "text")) {
    fail("Could not send multipart fields");
    abortUpload();
    return false;
  }

  String fileHeader;
  fileHeader.reserve(boundary_.length() + 120);
  fileHeader += "--"; fileHeader += boundary_;
  fileHeader += "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
  if (!sendTextChunk(fileHeader)) {
    fail("Could not send WAV multipart header");
    abortUpload();
    return false;
  }
  uint8_t wavHeader[44];
  makeWavHeader(wavHeader);
  if (!sendAudioChunk(wavHeader, sizeof(wavHeader))) {
    fail("Could not send WAV header");
    abortUpload();
    return false;
  }
  return true;
}

bool GroqAudioStream::sendAudioChunk(const uint8_t *data, size_t length) {
  if (!streamActive_ || (!data && length)) return false;
  char sizeLine[20];
  int n = snprintf(sizeLine, sizeof(sizeLine), "%lX\r\n", static_cast<unsigned long>(length));
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(sizeLine)) return false;
  if (client_.write(reinterpret_cast<const uint8_t *>(sizeLine), static_cast<size_t>(n)) != static_cast<size_t>(n)) return false;
  if (length && client_.write(data, length) != length) return false;
  static const uint8_t crlf[] = {'\r', '\n'};
  return client_.write(crlf, sizeof(crlf)) == sizeof(crlf);
}

bool GroqAudioStream::sendTextChunk(const String &text) {
  return sendAudioChunk(reinterpret_cast<const uint8_t *>(text.c_str()), text.length());
}

bool GroqAudioStream::sendFormField(const char *name, const String &value) {
  String field;
  field.reserve(boundary_.length() + strlen(name) + value.length() + 80);
  field += "--"; field += boundary_; field += "\r\nContent-Disposition: form-data; name=\"";
  field += name; field += "\"\r\n\r\n"; field += value; field += "\r\n";
  return sendTextChunk(field);
}

bool GroqAudioStream::finishUpload(String &transcript) {
  transcript = "";
  String tail("\r\n--"); tail += boundary_; tail += "--\r\n";
  if (!sendTextChunk(tail)) {
    fail("Could not close multipart body"); abortUpload(); return false;
  }
  static const uint8_t finalChunk[] = {'0', '\r', '\n', '\r', '\n'};
  if (client_.write(finalChunk, sizeof(finalChunk)) != sizeof(finalChunk)) {
    fail("Could not finish HTTP chunked request"); abortUpload(); return false;
  }
  streamActive_ = false;
  String body;
  const bool ok = readResponse(body);
  client_.stop();
  if (!ok) return false;
  body.trim();
  if (httpStatus_ < 200 || httpStatus_ >= 300) {
    fail("Groq HTTP " + String(httpStatus_) + ": " + body);
    return false;
  }
  transcript = body;
  return true;
}

void GroqAudioStream::abortUpload() {
  client_.stop();
  streamActive_ = false;
}

void GroqAudioStream::makeWavHeader(uint8_t h[44]) const {
  const uint32_t unknownSize = 0xFFFFFFFFUL;
  const uint32_t sampleRate = config_.sampleRate;
  const uint32_t byteRate = sampleRate * 2UL;
  const uint32_t fmtLength = 16;
  const uint16_t pcm = 1, channels = 1, blockAlign = 2, bits = 16;
  memcpy(h, "RIFF", 4); memcpy(h + 4, &unknownSize, 4); memcpy(h + 8, "WAVEfmt ", 8);
  memcpy(h + 16, &fmtLength, 4); memcpy(h + 20, &pcm, 2); memcpy(h + 22, &channels, 2);
  memcpy(h + 24, &sampleRate, 4); memcpy(h + 28, &byteRate, 4); memcpy(h + 32, &blockAlign, 2);
  memcpy(h + 34, &bits, 2); memcpy(h + 36, "data", 4); memcpy(h + 40, &unknownSize, 4);
}

void GroqAudioStream::resetFilter() {
  hpX1_ = hpY1_ = hpX2_ = hpY2_ = 0.0f;
  const float cutoff = config_.highPassHz > 1.0f ? config_.highPassHz : 120.0f;
  const float rc = 1.0f / (2.0f * PI * cutoff);
  hpA_ = rc / (rc + 1.0f / static_cast<float>(config_.sampleRate));
}

uint32_t GroqAudioStream::filterAudio(int16_t *samples, size_t count) {
  uint32_t peak = 0;
  for (size_t i = 0; i < count; ++i) {
    const float v = static_cast<float>(samples[i]);
    const float p = hpA_ * (hpY1_ + v - hpX1_);
    hpX1_ = v; hpY1_ = p;
    const float q = hpA_ * (hpY2_ + p - hpX2_);
    hpX2_ = p; hpY2_ = q;
    const uint32_t magnitude = static_cast<uint32_t>(fabsf(q));
    if (magnitude > peak) peak = magnitude;
    int32_t amplified = static_cast<int32_t>(q * config_.audioGain);
    if (amplified > 32767) amplified = 32767;
    if (amplified < -32768) amplified = -32768;
    samples[i] = static_cast<int16_t>(amplified);
  }
  return peak;
}

bool GroqAudioStream::readLine(String &line, uint32_t timeoutMs) {
  line = "";
  const uint32_t start = millis();
  while (static_cast<uint32_t>(millis() - start) < timeoutMs) {
    while (client_.available()) {
      int r = client_.read();
      if (r < 0) break;
      char c = static_cast<char>(r);
      if (c == '\n') return true;
      if (c != '\r' && line.length() < 1024) line += c;
    }
    if (!client_.connected() && !client_.available()) return line.length() > 0;
    delay(1);
  }
  return false;
}

bool GroqAudioStream::readResponse(String &body) {
  body = "";
  String line;
  if (!readLine(line, config_.groqTimeoutMs)) { fail("Timed out waiting for HTTP status"); return false; }
  int space = line.indexOf(' ');
  if (!line.startsWith("HTTP/") || space < 0) { fail("Invalid HTTP status line"); return false; }
  httpStatus_ = line.substring(space + 1, space + 4).toInt();
  bool chunked = false;
  long contentLength = -1;
  uint8_t count = 0;
  while (count++ < 64) {
    if (!readLine(line, config_.groqTimeoutMs)) { fail("Timed out reading HTTP headers"); return false; }
    if (!line.length()) break;
    String lower = line; lower.toLowerCase();
    if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") >= 0) chunked = true;
    if (lower.startsWith("content-length:")) {
      String value = line.substring(line.indexOf(':') + 1); value.trim(); contentLength = value.toInt();
    }
  }

  if (chunked) {
    while (body.length() < MAX_HTTP_RESPONSE_BYTES) {
      if (!readLine(line, config_.groqTimeoutMs)) { fail("Timed out reading response chunk size"); return false; }
      int semi = line.indexOf(';'); if (semi >= 0) line = line.substring(0, semi); line.trim();
      long chunkSize = strtol(line.c_str(), nullptr, 16);
      if (chunkSize < 0) { fail("Invalid HTTP response chunk size"); return false; }
      if (chunkSize == 0) {
        while (readLine(line, 1000) && line.length()) {}
        break;
      }
      const uint32_t chunkStart = millis();
      for (long i = 0; i < chunkSize; ++i) {
        while (!client_.available()) {
          if (!client_.connected()) { fail("Connection ended inside HTTP chunk"); return false; }
          if (static_cast<uint32_t>(millis() - chunkStart) >= config_.groqTimeoutMs) { fail("Timed out inside HTTP chunk"); return false; }
          delay(1);
        }
        int r = client_.read(); if (r >= 0) body += static_cast<char>(r);
        if (body.length() >= MAX_HTTP_RESPONSE_BYTES) { fail("Groq response too large"); return false; }
      }
      if (!readLine(line, config_.groqTimeoutMs)) { fail("Missing HTTP chunk terminator"); return false; }
    }
  } else if (contentLength >= 0) {
    if (static_cast<size_t>(contentLength) > MAX_HTTP_RESPONSE_BYTES) { fail("Groq response too large"); return false; }
    const uint32_t started = millis();
    while (body.length() < static_cast<size_t>(contentLength)) {
      if (client_.available()) { int r = client_.read(); if (r >= 0) body += static_cast<char>(r); }
      else {
        if (!client_.connected()) { fail("Connection ended before full response"); return false; }
        if (static_cast<uint32_t>(millis() - started) >= config_.groqTimeoutMs) { fail("Timed out reading response body"); return false; }
        delay(1);
      }
    }
  } else {
    const uint32_t started = millis();
    while (client_.connected() || client_.available()) {
      if (client_.available()) { int r = client_.read(); if (r >= 0) body += static_cast<char>(r); }
      else {
        if (static_cast<uint32_t>(millis() - started) >= config_.groqTimeoutMs) { fail("Timed out reading response"); return false; }
        delay(1);
      }
      if (body.length() >= MAX_HTTP_RESPONSE_BYTES) { fail("Groq response too large"); return false; }
    }
  }
  return true;
}

void GroqAudioStream::fail(const String &message) {
  lastError_ = message;
  if (config_.debugOutput) {
    Serial.print("[GroqAudioStream] ");
    Serial.println(message);
  }
}
