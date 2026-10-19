#include "WakeNet9Arduino.h"

#include <string.h>
#include "model_path.h"
#include "esp_wn_models.h"
#include "esp_wn_iface.h"

WakeNet9Arduino::WakeNet9Arduino()
    : models_(nullptr), wakeData_(nullptr), wakeInterface_(nullptr),
      wakeModelName_(nullptr), chunkSize_(0), active_(false),
      i2sStarted_(false), sampleRate_(16000), bclkPin_(17), wsPin_(15),
      dataPin_(16), useLeftChannel_(true), i2sTimeoutMs_(100), callback_(nullptr) {}

WakeNet9Arduino::~WakeNet9Arduino() {
  end();
}

void WakeNet9Arduino::setSerialOutput(bool enabled) {
  (void)enabled;
}

void WakeNet9Arduino::setWakeWordCallback(WakeWordCallback callback) {
  callback_ = callback;
}

bool WakeNet9Arduino::beginAudio() {
  i2s_.setTimeout(i2sTimeoutMs_);
  i2s_.setPins(bclkPin_, wsPin_, -1, dataPin_, -1);

  if (!i2s_.begin(I2S_MODE_STD, sampleRate_, I2S_DATA_BIT_WIDTH_32BIT,
                  I2S_SLOT_MODE_MONO,
                  useLeftChannel_ ? I2S_STD_SLOT_LEFT : I2S_STD_SLOT_RIGHT)) {
    i2s_.end();
    return false;
  }

  if (!i2s_.configureRX(sampleRate_, I2S_DATA_BIT_WIDTH_32BIT,
                        I2S_SLOT_MODE_MONO, I2S_RX_TRANSFORM_32_TO_16)) {
    i2s_.end();
    return false;
  }

  i2sStarted_ = true;
  return true;
}

bool WakeNet9Arduino::begin(const Config &config) {
  if (active_) end();
  if (!config.use32BitInput) return false;

  if (config.wakeWordLabel) {
    String label(config.wakeWordLabel);
    label.trim();
    label.toUpperCase();
    if (label != "HI ESP") return false;
  }

  bclkPin_ = config.bclkPin;
  wsPin_ = config.wsPin;
  dataPin_ = config.dataPin;
  sampleRate_ = config.sampleRate;
  useLeftChannel_ = config.useLeftChannel;
  i2sTimeoutMs_ = config.i2sTimeoutMs < 10 ? 10 : config.i2sTimeoutMs;

  if (sampleRate_ != 16000) return false;

  if (!models_) {
    models_ = esp_srmodel_init("model");
    if (!models_) return false;
  }

  if (!wakeModelName_) {
    wakeModelName_ = esp_srmodel_filter(
        static_cast<srmodel_list_t *>(models_), ESP_WN_PREFIX, "hiesp");
    if (!wakeModelName_) return false;
  }

  if (!wakeInterface_) {
    wakeInterface_ = esp_wn_handle_from_name(wakeModelName_);
    if (!wakeInterface_) return false;
  }

  const esp_wn_iface_t *wn = static_cast<const esp_wn_iface_t *>(wakeInterface_);
  wakeData_ = wn->create(wakeModelName_, DET_MODE_90);
  if (!wakeData_) return false;

  chunkSize_ = wn->get_samp_chunksize(static_cast<model_iface_data_t *>(wakeData_));
  if (chunkSize_ <= 0 || chunkSize_ > 1024) {
    wn->destroy(static_cast<model_iface_data_t *>(wakeData_));
    wakeData_ = nullptr;
    chunkSize_ = 0;
    return false;
  }

  if (!beginAudio()) {
    wn->destroy(static_cast<model_iface_data_t *>(wakeData_));
    wakeData_ = nullptr;
    chunkSize_ = 0;
    return false;
  }

  active_ = true;
  return true;
}

bool WakeNet9Arduino::end() {
  active_ = false;
  if (i2sStarted_) {
    i2s_.end();
    i2sStarted_ = false;
  }

  if (wakeData_ && wakeInterface_) {
    const esp_wn_iface_t *wn = static_cast<const esp_wn_iface_t *>(wakeInterface_);
    wn->destroy(static_cast<model_iface_data_t *>(wakeData_));
    wakeData_ = nullptr;
  }
  chunkSize_ = 0;
  return true;
}

void WakeNet9Arduino::update() {
  if (!active_ || !wakeData_ || !wakeInterface_ || chunkSize_ <= 0) return;

  const size_t expected = static_cast<size_t>(chunkSize_) * sizeof(int16_t);
  const size_t received = i2s_.readBytes(reinterpret_cast<char *>(audioBuffer_), expected);
  if (received != expected) return;

  const esp_wn_iface_t *wn = static_cast<const esp_wn_iface_t *>(wakeInterface_);
  const int detected = wn->detect(static_cast<model_iface_data_t *>(wakeData_), audioBuffer_);
  if (detected > 0 && callback_) callback_();
}
