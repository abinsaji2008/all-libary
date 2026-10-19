#include "RGBLED.h"

#ifndef RGB_BUILTIN
#error "Select a board variant that defines RGB_BUILTIN and supports rgbLedWrite()."
#endif

namespace {
constexpr uint8_t LED_BRIGHTNESS = 48;
constexpr uint32_t BLINK_INTERVAL_MS = 280;
constexpr uint32_t TASK_INTERVAL_MS = 20;
}

bool RGBLED::begin() {
  if (started_) {
    setState(State::OFF);
    return true;
  }
  requestedState_ = static_cast<uint8_t>(State::OFF);
  rgbLedWrite(RGB_BUILTIN, 0, 0, 0);

  const BaseType_t created = xTaskCreatePinnedToCore(
      taskEntry, "RGBLED", 2048, this, 1, &taskHandle_, 1);
  if (created != pdPASS) {
    taskHandle_ = nullptr;
    return false;
  }
  started_ = true;
  return true;
}

void RGBLED::setState(State stateValue) {
  portENTER_CRITICAL(&stateMux_);
  requestedState_ = static_cast<uint8_t>(stateValue);
  portEXIT_CRITICAL(&stateMux_);
}

RGBLED::State RGBLED::state() const {
  return readState();
}

RGBLED::State RGBLED::readState() const {
  uint8_t value;
  portMUX_TYPE *mux = const_cast<portMUX_TYPE *>(&stateMux_);
  portENTER_CRITICAL(mux);
  value = requestedState_;
  portEXIT_CRITICAL(mux);
  if (value > static_cast<uint8_t>(State::PROCESSING_GREEN)) return State::OFF;
  return static_cast<State>(value);
}

void RGBLED::taskEntry(void *argument) {
  static_cast<RGBLED *>(argument)->taskLoop();
}

void RGBLED::taskLoop() {
  State previousState = State::OFF;
  bool blueOn = false;
  uint32_t lastBlink = millis();
  for (;;) {
    const State currentState = readState();
    if (currentState != previousState) {
      previousState = currentState;
      blueOn = false;
      lastBlink = millis();
      if (currentState == State::PROCESSING_GREEN) {
        rgbLedWrite(RGB_BUILTIN, 0, LED_BRIGHTNESS, 0);
      } else {
        rgbLedWrite(RGB_BUILTIN, 0, 0, 0);
      }
    }
    if (currentState == State::RECORDING_BLUE_BLINK &&
        static_cast<uint32_t>(millis() - lastBlink) >= BLINK_INTERVAL_MS) {
      lastBlink = millis();
      blueOn = !blueOn;
      rgbLedWrite(RGB_BUILTIN, 0, 0, blueOn ? LED_BRIGHTNESS : 0);
    }
    vTaskDelay(pdMS_TO_TICKS(TASK_INTERVAL_MS));
  }
}
