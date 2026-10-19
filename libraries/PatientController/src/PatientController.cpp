#include "PatientController.h"
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

bool PatientController::begin(HardwareSerial &port,
                              int rxPin,
                              int txPin,
                              uint32_t baud,
                              uint32_t acknowledgementTimeoutMs) {
  port_ = &port;
  acknowledgementTimeoutMs_ = acknowledgementTimeoutMs;
  port_->begin(baud, SERIAL_8N1, rxPin, txPin);
  port_->setTimeout(100);
  return true;
}

PatientController::Result PatientController::sendRequest(
    const String &action,
    const String &contact,
    const String &phoneNumber,
    const String &message) {
  if (!port_) {
    return {false, "failed", "The controller UART has not been initialized."};
  }
  while (port_->available()) (void)port_->read();

  const uint32_t requestId = nextRequestId_++;
  DynamicJsonDocument request(1000 + message.length() * 2);
  request["id"] = requestId;
  request["action"] = action;
  if (contact.length()) request["contact"] = contact;
  if (phoneNumber.length()) request["number"] = phoneNumber;
  if (message.length()) request["message"] = message;

  String requestLine;
  requestLine.reserve(400 + message.length() * 2);
  serializeJson(request, requestLine);
  port_->println(requestLine);

  String replyLine;
  replyLine.reserve(300);
  const uint32_t start = millis();
  while (millis() - start < acknowledgementTimeoutMs_) {
    while (port_->available()) {
      const char c = static_cast<char>(port_->read());
      if (c == '\r') continue;
      if (c == '\n') {
        if (replyLine.isEmpty()) continue;
        DynamicJsonDocument acknowledgement(1024);
        const bool parsed = !deserializeJson(acknowledgement, replyLine);
        replyLine = "";
        if (parsed && static_cast<uint32_t>(acknowledgement["id"] | 0UL) == requestId) {
          const bool success = acknowledgement["success"] | false;
          const String status = acknowledgement["status"] | (success ? "success" : "failed");
          const String details = acknowledgement["message"] |
              (success ? "Controller confirmed the action." : "Controller reported failure.");
          return {success, status, details};
        }
      } else if (replyLine.length() < 512) {
        replyLine += c;
      } else {
        replyLine = "";
      }
    }
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return {false, "timeout", "No matching confirmation arrived from the secondary ESP32."};
}
