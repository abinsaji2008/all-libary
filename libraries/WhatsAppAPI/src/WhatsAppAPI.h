#ifndef WHATSAPP_API_H
#define WHATSAPP_API_H

#include <Arduino.h>

class WhatsAppAPI {
public:
  struct Result {
    bool success = false;
    int httpStatus = 0;
    String status = "failed";
    String message;
    String error;
  };

  bool begin(const char *apiBaseUrl, const char *instanceName, const char *apiKey);
  Result sendText(const String &phoneNumber, const String &text);
  String lastError() const { return lastError_; }
  int lastHttpStatus() const { return lastHttpStatus_; }

private:
  String apiBaseUrl_;
  String instanceName_;
  String apiKey_;
  String lastError_;
  int lastHttpStatus_ = 0;
  bool ready_ = false;
  String normalizePhone(const String &phone) const;
};

#endif
