#include "GroqTTS.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ESP_I2S.h>
#include <esp_heap_caps.h>
#include <esp32-hal-psram.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

namespace {
constexpr char TTS_ENDPOINT[] = "https://api.groq.com/openai/v1/audio/speech";
constexpr char DEFAULT_MODEL[] = "canopylabs/orpheus-v1-english";
constexpr char DEFAULT_VOICE[] = "troy";
constexpr size_t MAX_WAV_BYTES = 3U * 1024U * 1024U;
constexpr size_t FALLBACK_WAV_BYTES = 512U * 1024U;
constexpr size_t IO_CHUNK_BYTES = 4096U;
constexpr size_t MONO_SAMPLES_PER_BLOCK = 512U;
constexpr size_t MAX_TTS_INPUT_CHARS = 200U;
constexpr uint32_t HTTP_TIMEOUT_MS = 45000U;

class WavCaptureStream : public Stream {
public:
  WavCaptureStream() : buffer_(nullptr), capacity_(0), length_(0), overflow_(false) {
    if (psramFound()) {
      buffer_ = static_cast<uint8_t *>(heap_caps_malloc(MAX_WAV_BYTES,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
      if (buffer_) capacity_ = MAX_WAV_BYTES;
    }
    if (!buffer_) {
      buffer_ = static_cast<uint8_t *>(malloc(FALLBACK_WAV_BYTES));
      if (buffer_) capacity_ = FALLBACK_WAV_BYTES;
    }
  }

  ~WavCaptureStream() override { clear(); }

  bool valid() const { return buffer_ != nullptr; }
  uint8_t *data() const { return buffer_; }
  size_t size() const { return length_; }
  size_t capacity() const { return capacity_; }
  bool overflowed() const { return overflow_; }

  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}

  size_t write(uint8_t value) override {
    if (!buffer_ || length_ >= capacity_) {
      overflow_ = true;
      return 0;
    }
    buffer_[length_++] = value;
    return 1;
  }

  size_t write(const uint8_t *data, size_t count) override {
    if (!buffer_ || (!data && count)) return 0;
    if (count > capacity_ - length_) {
      overflow_ = true;
      return 0;
    }
    memcpy(buffer_ + length_, data, count);
    length_ += count;
    return count;
  }

  uint8_t *release(size_t &length) {
    length = length_;
    uint8_t *result = buffer_;
    buffer_ = nullptr;
    capacity_ = 0;
    length_ = 0;
    return result;
  }

private:
  void clear() {
    if (buffer_) free(buffer_);
    buffer_ = nullptr;
    capacity_ = 0;
    length_ = 0;
  }

  uint8_t *buffer_;
  size_t capacity_;
  size_t length_;
  bool overflow_;
};

static String jsonEscape(const String &input) {
  String out;
  out.reserve(input.length() + 16);
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<uint8_t>(c) < 0x20) {
          // Drop unsupported control characters from spoken input.
        } else {
          out += c;
        }
        break;
    }
  }
  return out;
}

static uint16_t readLE16(const uint8_t *p) {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t readLE32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

struct WavInfo {
  uint16_t format = 0;
  uint16_t channels = 0;
  uint32_t sampleRate = 0;
  uint16_t bitsPerSample = 0;
  size_t dataOffset = 0;
  size_t dataLength = 0;
};

static bool parseWav(const uint8_t *wav, size_t length, WavInfo &info) {
  if (!wav || length < 44 || memcmp(wav, "RIFF", 4) != 0 ||
      memcmp(wav + 8, "WAVE", 4) != 0) return false;

  bool foundFmt = false;
  bool foundData = false;
  size_t offset = 12;

  while (offset + 8 <= length) {
    const uint8_t *chunk = wav + offset;
    const uint32_t chunkLength32 = readLE32(chunk + 4);
    const size_t chunkStart = offset + 8;
    if (chunkStart > length) break;
    const size_t availableChunk = length - chunkStart;
    const size_t chunkLength = (chunkLength32 < availableChunk)
                                 ? static_cast<size_t>(chunkLength32)
                                 : availableChunk;

    if (memcmp(chunk, "fmt ", 4) == 0) {
      if (chunkLength < 16) return false;
      info.format = readLE16(wav + chunkStart);
      info.channels = readLE16(wav + chunkStart + 2);
      info.sampleRate = readLE32(wav + chunkStart + 4);
      info.bitsPerSample = readLE16(wav + chunkStart + 14);
      foundFmt = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      info.dataOffset = chunkStart;
      info.dataLength = chunkLength;
      foundData = true;
    }

    // RIFF chunks are word-aligned; avoid overflow before advancing.
    const uint64_t next = static_cast<uint64_t>(chunkStart) + chunkLength32 + (chunkLength32 & 1U);
    if (next <= offset || next > length) break;
    offset = static_cast<size_t>(next);
  }

  if (!foundFmt || !foundData) return false;
  if (info.format != 1 || info.bitsPerSample != 16) return false;
  if (info.channels != 1 && info.channels != 2) return false;
  if (info.sampleRate < 8000 || info.sampleRate > 48000) return false;
  if (info.dataLength < 2) return false;

  const size_t frameBytes = static_cast<size_t>(info.channels) * 2U;
  info.dataLength -= info.dataLength % frameBytes;
  return info.dataLength > 0;
}

static bool writeAll(I2SClass &i2s, const uint8_t *data, size_t length) {
  size_t offset = 0;
  unsigned int stalled = 0;
  while (offset < length) {
    const size_t written = i2s.write(data + offset, length - offset);
    if (written > 0) {
      offset += written;
      stalled = 0;
      continue;
    }
    if (++stalled > 100) return false;
    delay(1);
  }
  return true;
}
} // namespace

GroqTTS::GroqTTS()
    : model_(DEFAULT_MODEL), voice_(DEFAULT_VOICE), httpStatus_(0),
      bclkPin_(7), wsPin_(8), dataPin_(9), volumePercent_(50), ready_(false) {}

bool GroqTTS::begin(const char *apiKey) {
  lastError_ = "";
  ready_ = false;
  if (!apiKey || !apiKey[0]) {
    fail("Groq API key is missing.");
    return false;
  }
  apiKey_ = apiKey;
  if (model_.isEmpty()) model_ = DEFAULT_MODEL;
  if (voice_.isEmpty()) voice_ = DEFAULT_VOICE;
  ready_ = true;
  return true;
}

void GroqTTS::setPins(int8_t bclkPin, int8_t wsPin, int8_t dataPin) {
  bclkPin_ = bclkPin;
  wsPin_ = wsPin;
  dataPin_ = dataPin;
}

void GroqTTS::setVoice(const char *voice) {
  if (voice && voice[0]) voice_ = voice;
}

void GroqTTS::setModel(const char *model) {
  if (model && model[0]) model_ = model;
}

void GroqTTS::setVolume(uint8_t percent) {
  volumePercent_ = percent > 100 ? 100 : percent;
}

bool GroqTTS::say(const String &input) {
  lastError_ = "";
  httpStatus_ = 0;
  if (!ready_) {
    fail("Call begin(apiKey) before say().");
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    fail("Wi-Fi is not connected.");
    return false;
  }

  String text = input;
  text.trim();
  if (text.isEmpty()) {
    fail("No text supplied to speak.");
    return false;
  }

  // Groq Orpheus TTS accepts a maximum 200-character input per request.
  // Split at word boundaries so longer assistant responses remain speakable.
  size_t start = 0;
  while (start < text.length()) {
    size_t remaining = text.length() - start;
    size_t take = remaining <= MAX_TTS_INPUT_CHARS ? remaining : MAX_TTS_INPUT_CHARS;
    if (start + take < text.length()) {
      int split = -1;
      for (int i = static_cast<int>(take) - 1; i > 0; --i) {
        if (text[start + static_cast<size_t>(i)] == ' ') {
          split = i;
          break;
        }
      }
      if (split > 0) take = static_cast<size_t>(split);
    }

    String chunk = text.substring(start, start + take);
    chunk.trim();
    if (chunk.length()) {
      uint8_t *wav = nullptr;
      size_t wavLength = 0;
      if (!requestWav(chunk, &wav, &wavLength)) {
        if (wav) free(wav);
        return false;
      }
      const bool played = playWav(wav, wavLength);
      free(wav);
      if (!played) return false;
    }

    start += take;
    while (start < text.length() && text[start] == ' ') ++start;
  }
  return true;
}

bool GroqTTS::requestWav(const String &text, uint8_t **wav, size_t *wavLength) {
  if (!wav || !wavLength) {
    fail("Internal error: invalid WAV output pointer.");
    return false;
  }
  *wav = nullptr;
  *wavLength = 0;

  if (text.length() > MAX_TTS_INPUT_CHARS) {
    fail("TTS input chunk exceeds 200 characters.");
    return false;
  }

  WiFiClientSecure tls;
  // Development setting, consistent with the existing prototype libraries.
  // Use CA certificate validation before deploying this in a real patient-care setting.
  tls.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(HTTP_TIMEOUT_MS);
  http.setReuse(false);
  if (!http.begin(tls, TTS_ENDPOINT)) {
    fail("Could not initialize Groq TTS HTTPS request.");
    return false;
  }

  http.addHeader("Authorization", String("Bearer ") + apiKey_);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Accept", "audio/wav");
  http.addHeader("Accept-Encoding", "identity");

  String body;
  body.reserve(text.length() + model_.length() + voice_.length() + 100);
  body = "{\"model\":\"" + jsonEscape(model_) +
         "\",\"input\":\"" + jsonEscape(text) +
         "\",\"voice\":\"" + jsonEscape(voice_) +
         "\",\"response_format\":\"wav\"}";

  httpStatus_ = http.POST(body);
  if (httpStatus_ < 200 || httpStatus_ >= 300) {
    String errorBody = http.getString();
    if (errorBody.length() > 500) errorBody.remove(500);
    fail("Groq TTS HTTP " + String(httpStatus_) +
         (errorBody.isEmpty() ? String(".") : String(": ") + errorBody));
    http.end();
    return false;
  }

  WavCaptureStream capture;
  if (!capture.valid()) {
    fail("Not enough memory for WAV buffering. Enable PSRAM on the ESP32-S3.");
    http.end();
    return false;
  }

  const int declaredLength = http.getSize();
  if (declaredLength > static_cast<int>(capture.capacity())) {
    fail("Generated WAV exceeds the configured audio buffer capacity.");
    http.end();
    return false;
  }

  const int copied = http.writeToStream(&capture);
  http.end();

  if (capture.overflowed()) {
    fail("Generated WAV exceeded the available audio buffer.");
    return false;
  }
  if (copied < 0 || capture.size() < 44) {
    fail("Failed to download a complete WAV response from Groq.");
    return false;
  }
  if (declaredLength >= 0 && capture.size() != static_cast<size_t>(declaredLength)) {
    fail("Incomplete WAV download: expected " + String(declaredLength) +
         " bytes but received " + String(capture.size()) + ".");
    return false;
  }

  *wav = capture.release(*wavLength);
  return *wav != nullptr && *wavLength > 0;
}

bool GroqTTS::playWav(const uint8_t *wav, size_t wavLength) {
  WavInfo info;
  if (!parseWav(wav, wavLength, info)) {
    fail("Groq returned an unsupported or invalid WAV. Expected 16-bit PCM mono/stereo.");
    return false;
  }

  I2SClass speakerI2S;
  speakerI2S.setPins(bclkPin_, wsPin_, dataPin_, -1, -1);
  if (!speakerI2S.begin(I2S_MODE_STD, info.sampleRate,
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    speakerI2S.end();
    fail("I2S speaker initialization failed. Check GPIO 7/8/9 and MAX98357A wiring.");
    return false;
  }

  const uint8_t *pcm = wav + info.dataOffset;
  size_t remaining = info.dataLength;
  const uint16_t volume = volumePercent_;
  bool ok = true;

  if (info.channels == 2) {
    // Process stereo samples in bounded chunks and apply digital volume.
    alignas(4) int16_t samples[MONO_SAMPLES_PER_BLOCK * 2];
    while (remaining > 0) {
      size_t bytes = remaining;
      const size_t maxBytes = sizeof(samples);
      if (bytes > maxBytes) bytes = maxBytes;
      bytes -= bytes % 4U;
      if (!bytes) break;
      const size_t sampleCount = bytes / 2U;
      memcpy(samples, pcm, bytes);
      if (volume != 100) {
        for (size_t i = 0; i < sampleCount; ++i) {
          int32_t scaled = (static_cast<int32_t>(samples[i]) * volume) / 100;
          if (scaled > 32767) scaled = 32767;
          if (scaled < -32768) scaled = -32768;
          samples[i] = static_cast<int16_t>(scaled);
        }
      }
      if (!writeAll(speakerI2S, reinterpret_cast<const uint8_t *>(samples), bytes)) {
        ok = false;
        break;
      }
      pcm += bytes;
      remaining -= bytes;
    }
  } else {
    // Mono WAV: duplicate each source sample into left and right I2S slots.
    alignas(4) int16_t mono[MONO_SAMPLES_PER_BLOCK];
    alignas(4) int16_t stereo[MONO_SAMPLES_PER_BLOCK * 2];
    while (remaining > 0) {
      size_t bytes = remaining;
      if (bytes > sizeof(mono)) bytes = sizeof(mono);
      bytes -= bytes % 2U;
      if (!bytes) break;
      const size_t sampleCount = bytes / 2U;
      memcpy(mono, pcm, bytes);
      for (size_t i = 0; i < sampleCount; ++i) {
        int32_t scaled = (static_cast<int32_t>(mono[i]) * volume) / 100;
        if (scaled > 32767) scaled = 32767;
        if (scaled < -32768) scaled = -32768;
        const int16_t sample = static_cast<int16_t>(scaled);
        stereo[2U * i] = sample;
        stereo[2U * i + 1U] = sample;
      }
      const size_t outBytes = sampleCount * 4U;
      if (!writeAll(speakerI2S, reinterpret_cast<const uint8_t *>(stereo), outBytes)) {
        ok = false;
        break;
      }
      pcm += bytes;
      remaining -= bytes;
    }
  }

  // Push a short silence tail so the final queued samples drain before I2S stops.
  if (ok) {
    int16_t silence[256 * 2] = {};
    const size_t silenceFrames = info.sampleRate / 20U; // 50 ms
    size_t framesLeft = silenceFrames;
    while (framesLeft > 0) {
      const size_t frames = framesLeft > 256U ? 256U : framesLeft;
      if (!writeAll(speakerI2S, reinterpret_cast<const uint8_t *>(silence), frames * 4U)) {
        ok = false;
        break;
      }
      framesLeft -= frames;
    }
  }

  delay(30);
  speakerI2S.end();
  if (!ok) fail("I2S speaker playback failed while writing PCM samples.");
  return ok;
}

void GroqTTS::fail(const String &message) {
  lastError_ = message;
  Serial.print("[GroqTTS] ");
  Serial.println(message);
}