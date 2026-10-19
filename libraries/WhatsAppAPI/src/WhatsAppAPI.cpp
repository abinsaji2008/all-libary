#include "WhatsAppAPI.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

namespace {
constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t REQUEST_TIMEOUT_MS = 20000;
}

bool WhatsAppAPI::begin(const char *apiBaseUrl, const char *instanceName, const char *apiKey) {
  ready_ = false; lastError_ = ""; lastHttpStatus_ = 0;
  if (!apiBaseUrl || !apiBaseUrl[0] || !instanceName || !instanceName[0] || !apiKey || !apiKey[0]) {
    lastError_ = "Evolution API URL, instance, or API key is missing in secrets.h.";
    return false;
  }
  apiBaseUrl_ = apiBaseUrl; apiBaseUrl_.trim();
  while (apiBaseUrl_.endsWith("/")) apiBaseUrl_.remove(apiBaseUrl_.length() - 1);
  instanceName_ = instanceName; instanceName_.trim();
  apiKey_ = apiKey; apiKey_.trim();
  const bool placeholders = apiBaseUrl_.indexOf("YOUR-") >= 0 ||
                            apiBaseUrl_.indexOf("YOUR_") >= 0 ||
                            instanceName_.indexOf("YOUR_") >= 0 ||
                            apiKey_.indexOf("YOUR_") >= 0;
  ready_ = !apiBaseUrl_.isEmpty() && !instanceName_.isEmpty() && !apiKey_.isEmpty() && !placeholders;
  if (!ready_) lastError_ = "Evolution API configuration is incomplete.";
  return ready_;
}

String WhatsAppAPI::normalizePhone(const String &phone) const {
  String digits; digits.reserve(phone.length());
  for (size_t i = 0; i < phone.length(); ++i) {
    const char c = phone[i];
    if (c >= '0' && c <= '9') digits += c;
    else if (c == '+' && i == 0) continue;
    else if (c == ' ' || c == '-' || c == '(' || c == ')' || c == '.') continue;
    else return String();
  }
  if (digits.length() < 8 || digits.length() > 15) return String();
  return digits;
}

WhatsAppAPI::Result WhatsAppAPI::sendText(const String &phoneNumber, const String &text) {
  Result result;
  lastError_ = ""; lastHttpStatus_ = 0;
  if (!ready_) { result.error = lastError_ = "WhatsAppAPI.begin() has not succeeded."; return result; }
  if (WiFi.status() != WL_CONNECTED) { result.error = lastError_ = "Wi-Fi is not connected."; return result; }
  const String number = normalizePhone(phoneNumber);
  String cleanText = text; cleanText.trim();
  if (number.isEmpty()) { result.error = lastError_ = "Contact phone number is invalid; use international digits."; return result; }
  if (cleanText.isEmpty()) { result.error = lastError_ = "WhatsApp message text is empty."; return result; }
  if (cleanText.length() > 1000) cleanText.remove(1000);

  String url = apiBaseUrl_ + "/message/sendText/" + instanceName_;
  WiFiClientSecure tls;
  // Development TLS setting; validate the server certificate before production use.
  tls.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(REQUEST_TIMEOUT_MS);
  if (!http.begin(tls, url)) {
    result.error = lastError_ = "Could not initialize Evolution API HTTPS request.";
    return result;
  }
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", apiKey_);
  DynamicJsonDocument request(2048 + cleanText.length() * 2);
  request["number"] = number;
  request["text"] = cleanText;
  String body; body.reserve(512 + cleanText.length() * 2);
  serializeJson(request, body);

  lastHttpStatus_ = http.POST(body);
  result.httpStatus = lastHttpStatus_;
  String responseBody = http.getString();
  if (lastHttpStatus_ >= 200 && lastHttpStatus_ < 300) {
    result.success = true;
    result.status = "accepted";
    result.message = "Evolution API accepted the message submission. Delivery is not confirmed.";
  } else if (lastHttpStatus_ <= 0) {
    result.error = String("Evolution API transport error: ") + HTTPClient::errorToString(lastHttpStatus_);
  } else {
    result.error = String("Evolution API returned HTTP ") + lastHttpStatus_;
    DynamicJsonDocument errDoc(2048);
    if (!deserializeJson(errDoc, responseBody)) {
      const char *message = errDoc["message"] | "";
      if (!message[0]) message = errDoc["error"] | "";
      if (message[0]) { result.error += ": "; result.error += message; }
    }
  }
  if (!result.success) {
    result.status = "failed";
    lastError_ = result.error;
  }
  http.end();
  return result;
}
