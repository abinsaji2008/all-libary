#ifndef PATIENT_CONTROLLER_H
#define PATIENT_CONTROLLER_H

#include <Arduino.h>

class PatientController {
public:
  struct Result {
    bool success = false;
    String status = "failed";
    String message;

    Result() = default;
    Result(bool ok, const String &state, const String &details)
        : success(ok), status(state), message(details) {}
  };

  bool begin(HardwareSerial &port,
             int rxPin = 44,
             int txPin = 43,
             uint32_t baud = 115200,
             uint32_t acknowledgementTimeoutMs = 1800);

  Result sendRequest(const String &action,
                     const String &contact = "",
                     const String &phoneNumber = "",
                     const String &message = "");

private:
  HardwareSerial *port_ = nullptr;
  uint32_t acknowledgementTimeoutMs_ = 1800;
  uint32_t nextRequestId_ = 1;
};

#endif
