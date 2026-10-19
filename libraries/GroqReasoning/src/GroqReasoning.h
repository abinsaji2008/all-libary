#ifndef GROQ_REASONING_H
#define GROQ_REASONING_H

#include <Arduino.h>
#include <time.h>

class GroqReasoning {
public:
  struct DeviceMapping {
    const char *name;
    const char *code;
  };

  struct ContactMapping {
    const char *name;
    const char *aliases;
    const char *number;
    ContactMapping(const char *n = nullptr, const char *a = nullptr,
                   const char *p = nullptr)
        : name(n), aliases(a), number(p) {}
  };

  struct ContactNumberMapping {
    const char *name;
    const char *number;
  };

  struct ContactPhoneMapping {
    const char *name;
    const char *number;
  };

  struct WhatsAppDraft {
    bool send = false;
    bool submitted = false;
    String contact;
    String number;
    String text;
    String error;
    String apiStatus;
    bool scheduled = false;
    String scheduledFor;
    String toJson() const;
  };

  struct WhatsAppConfig {
    const char *apiUrl;
    const char *instance;
    const char *apiKey;
  };

  struct Result {
    String response;
    String action;
    String toJson() const;
  };

  bool begin(const char *apiKey);
  bool begin(const char *apiKey, const char *model);
  bool begin(const String &apiKey) { return begin(apiKey.c_str()); }
  bool begin(const String &apiKey, const String &model) {
    return begin(apiKey.c_str(), model.c_str());
  }

  Result ask(const String &transcript,
             const DeviceMapping *devices,
             size_t deviceCount,
             const ContactMapping *contacts = nullptr,
             size_t contactCount = 0,
             const char *currentDateTime = nullptr);

  bool looksLikeWhatsAppRequest(const String &transcript) const;
  bool looksLikeWhatsAppRequest(const String &transcript,
                                const ContactMapping *contacts,
                                size_t contactCount) const;

  bool generateWhatsAppDraft(const String &transcript,
                             const ContactMapping *contacts,
                             size_t contactCount,
                             const ContactNumberMapping *phoneNumbers,
                             size_t phoneNumberCount,
                             WhatsAppDraft &draft,
                             const char *currentDateTime = nullptr);

  bool generateWhatsAppDraft(const String &transcript,
                             const ContactMapping *contacts,
                             size_t contactCount,
                             const ContactPhoneMapping *phoneNumbers,
                             size_t phoneNumberCount,
                             WhatsAppDraft &draft,
                             const char *currentDateTime = nullptr);

  // Convenience overload: phone number is stored alongside each contact.
  bool generateWhatsAppDraft(const String &transcript,
                             const ContactMapping *contacts,
                             size_t contactCount,
                             WhatsAppDraft &draft,
                             const char *currentDateTime = nullptr);

  // End-to-end voice request routing: WhatsApp, scheduling, or normal Groq reasoning.
  Result processTranscript(const String &transcript,
                           const DeviceMapping *devices,
                           size_t deviceCount,
                           const ContactMapping *contacts,
                           size_t contactCount,
                           const WhatsAppConfig &whatsappConfig);

  // NTP time helpers (India Standard Time).
  bool syncIndianClock(bool waitForSync = true);
  String currentIndianTimeContext() const;
  void serviceScheduledMessages(const WhatsAppConfig &whatsappConfig);

  bool sendWhatsAppMessage(WhatsAppDraft &draft,
                           const WhatsAppConfig &config);

  bool sendWhatsAppMessage(WhatsAppDraft &draft,
                           const char *apiBaseUrl,
                           const char *instanceName,
                           const char *apiKey);

  String lastError() const { return lastError_; }
  int lastHttpCode() const { return lastHttpCode_; }

private:
  int findContact(const String &person,
                  const ContactMapping *contacts,
                  size_t contactCount) const;

  bool isAllowedAction(const String &action,
                       const DeviceMapping *devices,
                       size_t deviceCount) const;

  String apiKey_;
  String model_;
  String lastError_;
  int lastHttpCode_ = 0;
  bool initialized_ = false;

  bool scheduleLoaded_ = false;
  bool scheduledMessagePending_ = false;
  time_t scheduledMessageEpoch_ = 0;
  String scheduledMessageContact_;
  String scheduledMessageNumber_;
  String scheduledMessageText_;
  unsigned long lastScheduledSendAttemptMs_ = 0;
  unsigned long lastNtpAttemptMs_ = 0;
};

#endif
