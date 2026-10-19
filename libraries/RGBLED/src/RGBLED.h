#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class RGBLED {
public:
  enum class State : uint8_t {
    OFF,
    RECORDING_BLUE_BLINK,
    PROCESSING_GREEN
  };

  RGBLED() = default;
  bool begin();
  void setState(State state);
  State state() const;

private:
  static void taskEntry(void *argument);
  void taskLoop();
  State readState() const;

  volatile uint8_t requestedState_ = static_cast<uint8_t>(State::OFF);
  TaskHandle_t taskHandle_ = nullptr;
  bool started_ = false;
  portMUX_TYPE stateMux_ = portMUX_INITIALIZER_UNLOCKED;
};
