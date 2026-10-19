#include "GroqReasoning.h"

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <new>
#include <Preferences.h>
#include <time.h>
#include <stdint.h>
#include <string.h>

namespace {
constexpr char CHAT_ENDPOINT[] = "https://api.groq.com/openai/v1/chat/completions";
constexpr char DEFAULT_MODEL[] = "openai/gpt-oss-20b";
constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;
constexpr uint32_t REQUEST_TIMEOUT_MS = 30000;
constexpr uint16_t MAX_COMPLETION_TOKENS = 256;

String normalizedName(String value) {
  value.trim();
  value.toLowerCase();
  while (value.length() > 0) {
    const char last = value[value.length() - 1];
    if (last == '.' || last == ',' || last == '!' || last == '?' ||
        last == ':' || last == ';' || last == '\'') {
      value.remove(value.length() - 1);
      value.trim();
    } else {
      break;
    }
  }
  while (value.length() > 0 &&
         (value[0] == '\'' || value[0] == '"')) {
    value.remove(0, 1);
    value.trim();
  }
  return value;
}

bool isCallAction(const String &action) {
  if (!action.startsWith("call:")) return false;
  String person = action.substring(5);
  person.trim();
  return !person.isEmpty() && person.length() <= 64;
}

String makeSystemPrompt(const GroqReasoning::DeviceMapping *devices,
                        size_t deviceCount,
                        const GroqReasoning::ContactMapping *contacts,
                        size_t contactCount,
                        const char *currentDateTime) {
  String prompt;
  prompt.reserve(1200 + deviceCount * 140 + contactCount * 100);
  prompt =
      "You are a concise voice assistant and intent router. Return ONLY a valid JSON object "
      "with exactly two string fields: response and action. No markdown or extra text. "
      "The response is a short natural sentence suitable for speaking. The action is one intent string. "
      "For normal conversation or questions, set action to none. Never invent device codes or phone numbers. "
      "The firmware validates every device action and contact locally. Do not claim an action or phone call "
      "has already completed.\n\n"
      "Special actions:\n"
      "- none: no supported action requested.\n"
      "- emergency_call: the user explicitly requests emergency calling.\n"
      "- refill_water: the user asks to refill water.\n"
      "- call:<person>: the user asks to call a person or role. For any call request, put the requested "
      "person/role after call:, even when that person is not listed below; local firmware will check availability. "
      "If it matches a listed contact or alias, use that contact's canonical name. Never create a phone number.\n\n"
      "Available devices and their only valid on/off actions:\n";

  bool printedDevice = false;
  if (devices != nullptr) {
    for (size_t i = 0; i < deviceCount; ++i) {
      if (devices[i].name == nullptr || devices[i].code == nullptr ||
          devices[i].name[0] == '\0' || devices[i].code[0] == '\0') continue;
      printedDevice = true;
      prompt += "- Device: \"";
      prompt += devices[i].name;
      prompt += "\"; on: \"";
      prompt += devices[i].code;
      prompt += "on\"; off: \"";
      prompt += devices[i].code;
      prompt += "off\".\n";
    }
  }
  if (!printedDevice) prompt += "- No devices are configured. Use action none for device requests.\n";

  prompt += "\nAllowed contacts (names and aliases only; phone numbers are handled separately by firmware):\n";
  bool printedContact = false;
  if (contacts != nullptr) {
    for (size_t i = 0; i < contactCount; ++i) {
      if (contacts[i].name == nullptr || contacts[i].name[0] == '\0') continue;
      printedContact = true;
      prompt += "- Contact: \"";
      prompt += contacts[i].name;
      prompt += "\"";
      if (contacts[i].aliases != nullptr && contacts[i].aliases[0] != '\0') {
        prompt += "; aliases: ";
        prompt += contacts[i].aliases;
      }
      prompt += "\n";
    }
  }
  if (!printedContact) prompt += "- No contacts are configured.\n";

  prompt +=
      "Examples: if primary light has base code l1, 'turn on primary light' maps to "
      "{\"response\":\"Turning on the primary light.\",\"action\":\"l1on\"}. "
      "If the user asks to call a person, make the action call:<person> and let firmware validate it. "
      "If the user asks for water, use refill_water. Always return both JSON fields.";

  if (currentDateTime != nullptr && currentDateTime[0] != '\0') {
    prompt += "\n\nCurrent system date and time: ";
    prompt += currentDateTime;
    prompt += ". This timestamp is authoritative and represents the current time in India Standard Time (IST, UTC+05:30). ";
    prompt += "For questions about the current time, date, day, or relative time such as today or tomorrow, answer using this timestamp. ";
    prompt += "Do not claim you do not know the current time when a timestamp is present. ";
    prompt += "For unrelated device commands, use this timestamp only if relevant and do not change the requested action. ";
  } else {
    prompt += "\n\nCurrent system date and time is unavailable. Do not guess the current time or date.";
  }

  return prompt;
}

String unavailableContactMessage(const String &person) {
  String label = person;
  label.trim();
  if (label.isEmpty()) label = "that person";
  return "Sorry, I can't contact " + label + " because that person isn't in my contact list.";
}
} // namespace

String GroqReasoning::Result::toJson() const {
  DynamicJsonDocument doc(256 + response.length() * 2 + action.length() * 2);
  doc["response"] = response;
  doc["action"] = action.length() ? action : "none";
  String json;
  serializeJson(doc, json);
  return json;
}

bool GroqReasoning::begin(const char *apiKey) {
  return begin(apiKey, DEFAULT_MODEL);
}

bool GroqReasoning::begin(const char *apiKey, const char *model) {
  lastError_ = "";
  lastHttpCode_ = 0;
  initialized_ = false;

  if (apiKey == nullptr || apiKey[0] == '\0') {
    lastError_ = "Groq API key is missing.";
    return false;
  }

  apiKey_ = apiKey;
  apiKey_.trim();
  if (apiKey_.isEmpty()) {
    lastError_ = "Groq API key is missing.";
    return false;
  }

  model_ = (model != nullptr && model[0] != '\0') ? model : DEFAULT_MODEL;
  model_.trim();
  if (model_.isEmpty()) model_ = DEFAULT_MODEL;

  initialized_ = true;
  return true;
}

int GroqReasoning::findContact(const String &person,
                               const ContactMapping *contacts,
                               size_t contactCount) const {
  if (contacts == nullptr) return -1;
  const String wanted = normalizedName(person);
  if (wanted.isEmpty()) return -1;

  for (size_t i = 0; i < contactCount; ++i) {
    if (contacts[i].name == nullptr || contacts[i].name[0] == '\0') continue;
    if (normalizedName(String(contacts[i].name)) == wanted) return static_cast<int>(i);

    if (contacts[i].aliases == nullptr || contacts[i].aliases[0] == '\0') continue;
    String aliases(contacts[i].aliases);
    int start = 0;
    while (start <= aliases.length()) {
      int separator = aliases.indexOf('|', start);
      if (separator < 0) separator = aliases.length();
      String alias = aliases.substring(start, separator);
      if (normalizedName(alias) == wanted) return static_cast<int>(i);
      if (separator >= aliases.length()) break;
      start = separator + 1;
    }
  }
  return -1;
}

bool GroqReasoning::isAllowedAction(const String &action,
                                   const DeviceMapping *devices,
                                   size_t deviceCount) const {
  if (action == "none" || action == "emergency_call" || action == "refill_water") {
    return true;
  }
  if (isCallAction(action)) return true; // Locally contact-validated after JSON parsing.

  if (devices == nullptr) return false;
  for (size_t i = 0; i < deviceCount; ++i) {
    if (devices[i].code == nullptr || devices[i].code[0] == '\0') continue;
    const String code(devices[i].code);
    if (action == code + "on" || action == code + "off") return true;
  }
  return false;
}

GroqReasoning::Result GroqReasoning::ask(const String &transcript,
                                         const DeviceMapping *devices,
                                         size_t deviceCount,
                                         const ContactMapping *contacts,
                                         size_t contactCount,
                                         const char *currentDateTime) {
  Result result;
  result.response = "Sorry, I couldn't process that request right now.";
  result.action = "none";
  lastError_ = "";
  lastHttpCode_ = 0;

  if (!initialized_) {
    lastError_ = "Call begin(apiKey) before ask().";
    return result;
  }

  String cleanTranscript = transcript;
  cleanTranscript.trim();
  if (cleanTranscript.isEmpty()) {
    result.response = "Sorry, I couldn't understand that.";
    lastError_ = "Transcript is empty.";
    return result;
  }

  if (WiFi.status() != WL_CONNECTED) {
    result.response = "I'm not connected to Wi-Fi right now.";
    lastError_ = "Wi-Fi is not connected.";
    return result;
  }

  WiFiClientSecure tls;
  // Development convenience. Configure certificate verification for production.
  tls.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(REQUEST_TIMEOUT_MS);

  if (!http.begin(tls, CHAT_ENDPOINT)) {
    result.response = "Sorry, I couldn't connect to the assistant service.";
    lastError_ = "Could not initialize the Groq HTTPS request.";
    return result;
  }

  http.addHeader("Authorization", String("Bearer ") + apiKey_);
  http.addHeader("Content-Type", "application/json");

  const String systemPrompt = makeSystemPrompt(
      devices, deviceCount, contacts, contactCount, currentDateTime);
  DynamicJsonDocument requestDoc(2048 + systemPrompt.length() * 2 + cleanTranscript.length() * 2);
  requestDoc["model"] = model_;
  requestDoc["max_completion_tokens"] = MAX_COMPLETION_TOKENS;
  requestDoc["reasoning_effort"] = "low";
  requestDoc["include_reasoning"] = false;
  JsonObject responseFormat = requestDoc.createNestedObject("response_format");
  responseFormat["type"] = "json_schema";
  JsonObject jsonSchema = responseFormat.createNestedObject("json_schema");
  jsonSchema["name"] = "assistant_intent";
  jsonSchema["strict"] = true;
  JsonObject schema = jsonSchema.createNestedObject("schema");
  schema["type"] = "object";
  JsonObject properties = schema.createNestedObject("properties");
  properties.createNestedObject("response")["type"] = "string";
  properties.createNestedObject("action")["type"] = "string";
  JsonArray required = schema.createNestedArray("required");
  required.add("response");
  required.add("action");
  schema["additionalProperties"] = false;

  JsonArray messages = requestDoc.createNestedArray("messages");
  JsonObject systemMessage = messages.createNestedObject();
  systemMessage["role"] = "system";
  systemMessage["content"] = systemPrompt;

  JsonObject userMessage = messages.createNestedObject();
  userMessage["role"] = "user";
  userMessage["content"] = cleanTranscript;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  lastHttpCode_ = http.POST(requestBody);
  String responseBody = http.getString();

  if (lastHttpCode_ >= 200 && lastHttpCode_ < 300) {
    DynamicJsonDocument responseDoc(8192);
    DeserializationError error = deserializeJson(responseDoc, responseBody);

    if (error) {
      result.response = "Sorry, I couldn't understand the assistant's reply.";
      lastError_ = String("Could not parse Groq response JSON: ") + error.c_str();
    } else {
      String content = responseDoc["choices"][0]["message"]["content"] | "";
      content.trim();

      if (content.isEmpty()) {
        result.response = "Sorry, the assistant returned an empty reply.";
        lastError_ = "Groq returned no message content.";
      } else {
        DynamicJsonDocument outputDoc(2048 + content.length() * 2);
        DeserializationError outputError = deserializeJson(outputDoc, content);
        if (outputError) {
          result.response = "Sorry, I couldn't process the assistant's reply.";
          lastError_ = String("Groq answer was not valid response/action JSON: ") + outputError.c_str();
        } else {
          result.response = outputDoc["response"] | "";
          result.action = outputDoc["action"] | "none";
          result.response.trim();
          result.action.trim();

          if (result.response.isEmpty()) {
            result.response = "Sorry, I couldn't process that request.";
            result.action = "none";
            lastError_ = "Groq JSON did not contain a response string.";
          } else if (!isAllowedAction(result.action, devices, deviceCount)) {
            result.action = "none";
            result.response = "Sorry, I couldn't safely process that action.";
            lastError_ = "Groq returned an unsupported action; changed to none.";
          } else if (isCallAction(result.action)) {
            const String requestedPerson = result.action.substring(5);
            const int contactIndex = findContact(requestedPerson, contacts, contactCount);

            if (contactIndex < 0) {
              result.response = unavailableContactMessage(requestedPerson);
              result.action = "none";
              lastError_ = "Requested contact is not in the contact list.";
            } else {
              result.action = String("call:") + contacts[contactIndex].name;
            }
          }
        }
      }
    }
  } else if (lastHttpCode_ <= 0) {
    result.response = "Sorry, I couldn't reach the assistant service.";
    lastError_ = String("Groq request failed: ") + HTTPClient::errorToString(lastHttpCode_);
  } else {
    result.response = "Sorry, the assistant service returned an error.";
    lastError_ = String("Groq API returned HTTP ") + lastHttpCode_;
    DynamicJsonDocument errorDoc(2048);
    if (!deserializeJson(errorDoc, responseBody)) {
      const char *message = errorDoc["error"]["message"] | "";
      if (message[0] != '\0') {
        lastError_ += ": ";
        lastError_ += message;
      }
      JsonVariant failedGeneration = errorDoc["error"]["failed_generation"];
      if (!failedGeneration.isNull()) {
        String details;
        serializeJson(failedGeneration, details);
        if (details.length() > 0) {
          lastError_ += " | failed_generation: ";
          lastError_ += details.substring(0, 300);
        }
      }
    }
  }

  http.end();
  return result;
}

String GroqReasoning::WhatsAppDraft::toJson() const {
  DynamicJsonDocument doc(768 + contact.length() * 2 + number.length() * 2 +
                          text.length() * 2 + error.length() * 2 +
                          scheduledFor.length() * 2);
  doc["send"] = send;
  doc["submitted"] = submitted;
  doc["contact"] = contact;
  doc["number"] = number;
  doc["text"] = text;
  doc["error"] = error;
  doc["apiStatus"] = apiStatus;
  doc["scheduled"] = scheduled;
  doc["scheduledFor"] = scheduledFor;
  String json;
  serializeJson(doc, json);
  return json;
}

bool GroqReasoning::looksLikeWhatsAppRequest(
    const String &transcript) const {
  return looksLikeWhatsAppRequest(transcript, nullptr, 0);
}

bool GroqReasoning::looksLikeWhatsAppRequest(
    const String &transcript,
    const ContactMapping *contacts,
    size_t contactCount) const {
  String text = transcript;
  text.toLowerCase();

  const bool explicitMessage =
      text.indexOf("whatsapp") >= 0 ||
      text.indexOf("message") >= 0 ||
      text.indexOf("text ") >= 0;

  const bool sendVerb =
      text.indexOf("send ") >= 0 ||
      text.indexOf("tell ") >= 0 ||
      text.indexOf("inform ") >= 0 ||
      text.indexOf("notify ") >= 0 ||
      text.indexOf("write to ") >= 0 ||
      text.indexOf("let ") >= 0;

  bool knownContact = false;
  if (contacts != nullptr) {
    for (size_t i = 0; i < contactCount && !knownContact; ++i) {
      if (contacts[i].name != nullptr && contacts[i].name[0] != '\0' &&
          text.indexOf(normalizedName(String(contacts[i].name))) >= 0) {
        knownContact = true;
        break;
      }

      if (contacts[i].aliases == nullptr || contacts[i].aliases[0] == '\0') continue;
      String aliases(contacts[i].aliases);
      int start = 0;
      while (start <= aliases.length()) {
        int separator = aliases.indexOf('|', start);
        if (separator < 0) separator = aliases.length();
        String alias = normalizedName(aliases.substring(start, separator));
        if (!alias.isEmpty() && text.indexOf(alias) >= 0) {
          knownContact = true;
          break;
        }
        if (separator >= aliases.length()) break;
        start = separator + 1;
      }
    }
  }

  return explicitMessage || (sendVerb && knownContact);
}

bool GroqReasoning::generateWhatsAppDraft(
    const String &transcript,
    const ContactMapping *contacts,
    size_t contactCount,
    const ContactNumberMapping *phoneNumbers,
    size_t phoneNumberCount,
    WhatsAppDraft &draft,
    const char *currentDateTime) {
  draft = WhatsAppDraft();
  lastError_ = "";
  lastHttpCode_ = 0;

  if (!initialized_) {
    lastError_ = "Call begin(apiKey) before generateWhatsAppDraft().";
    draft.error = lastError_;
    return false;
  }

  String cleanTranscript = transcript;
  cleanTranscript.trim();
  if (cleanTranscript.isEmpty()) {
    lastError_ = "Transcript is empty.";
    draft.error = lastError_;
    return false;
  }

  if (contacts == nullptr || contactCount == 0) {
    lastError_ = "No WhatsApp contacts are configured.";
    draft.error = lastError_;
    return false;
  }

  if (WiFi.status() != WL_CONNECTED) {
    lastError_ = "Wi-Fi is not connected.";
    draft.error = lastError_;
    return false;
  }

  String systemPrompt =
      "Prepare WhatsApp message drafts from voice commands. Return ONLY a valid JSON object "
      "with exactly three fields: send (boolean), contact (string), and text (string). "
      "Set send=true only when the user explicitly requests an outbound WhatsApp message AND "
      "clearly provides the message content. If a recipient is specified but no message content is "
      "provided, set send=false and return empty contact and text strings; never invent a generic greeting "
      "or infer what the user wants to say. "
      "Use only one of the listed canonical contacts, and return that canonical name, not an alias. "
      "Write a natural, concise message that preserves the user's intended meaning. Do not invent "
      "facts, times, promises, or events. Never generate, request, or return a phone number. "
      "Treat delivery instructions such as 'at 8 PM' or 'tomorrow at 9 AM' as scheduling metadata, not "
      "message text. Exclude those delivery instructions from the outgoing message body. "
      "Interpret the entire transcript, including earlier clauses. If the user says a sentence and then "
      "asks to send 'it', use that sentence as the message body. If the user says 'send hi', the message body "
      "is 'Hi'. Only ask for clarification when no message content is present anywhere in the transcript. "
      "Do not claim a message has already been sent. If the user did not explicitly request sending, or the "
      "recipient is not in the contact list, return "
      "{\"send\":false,\"contact\":\"\",\"text\":\"\"}. The text field must contain only "
      "the message body. Examples:\n"
      "Transcript: 'send hi to my mother' -> "
      "{\"send\":true,\"contact\":\"Mom\",\"text\":\"Hi\"}.\n"
      "Transcript: 'can you send hi to mother' -> "
      "{\"send\":true,\"contact\":\"Mom\",\"text\":\"Hi\"}.\n"
      "Transcript: 'I am not feeling well. Send it to my mother through WhatsApp.' -> "
      "{\"send\":true,\"contact\":\"Mom\",\"text\":\"I am not feeling well.\"}.\n"
      "Transcript: 'send a message to my mother' -> "
      "{\"send\":false,\"contact\":\"\",\"text\":\"\"}.\n"
      "Allowed contacts:\n";

  if (currentDateTime != nullptr && currentDateTime[0] != '\0') {
    systemPrompt += "\nCurrent system date and time: ";
    systemPrompt += currentDateTime;
    systemPrompt += ". This is the current time in India Standard Time (IST, UTC+05:30). ";
    systemPrompt += "Use it only for relative delivery timing and never copy the timestamp into message text unless requested by the user.";
  }

  for (size_t i = 0; i < contactCount; ++i) {
    if (contacts[i].name == nullptr || contacts[i].name[0] == '\0') continue;
    systemPrompt += "- Canonical name: \"";
    systemPrompt += contacts[i].name;
    systemPrompt += "\"";
    if (contacts[i].aliases != nullptr && contacts[i].aliases[0] != '\0') {
      systemPrompt += "; aliases: ";
      systemPrompt += contacts[i].aliases;
    }
    systemPrompt += "\n";
  }

  WiFiClientSecure tls;
  tls.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(REQUEST_TIMEOUT_MS);

  if (!http.begin(tls, CHAT_ENDPOINT)) {
    lastError_ = "Could not initialize the Groq WhatsApp-draft request.";
    draft.error = lastError_;
    return false;
  }

  http.addHeader("Authorization", String("Bearer ") + apiKey_);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument requestDoc(
      3072 + systemPrompt.length() * 2 + cleanTranscript.length() * 2);
  requestDoc["model"] = model_;
  requestDoc["max_completion_tokens"] = 300;
  requestDoc["reasoning_effort"] = "low";
  requestDoc["include_reasoning"] = false;
  JsonObject responseFormat = requestDoc.createNestedObject("response_format");
  responseFormat["type"] = "json_schema";
  JsonObject jsonSchema = responseFormat.createNestedObject("json_schema");
  jsonSchema["name"] = "whatsapp_draft";
  jsonSchema["strict"] = true;
  JsonObject schema = jsonSchema.createNestedObject("schema");
  schema["type"] = "object";
  JsonObject properties = schema.createNestedObject("properties");
  properties.createNestedObject("send")["type"] = "boolean";
  properties.createNestedObject("contact")["type"] = "string";
  properties.createNestedObject("text")["type"] = "string";
  JsonArray required = schema.createNestedArray("required");
  required.add("send");
  required.add("contact");
  required.add("text");
  schema["additionalProperties"] = false;

  JsonArray messages = requestDoc.createNestedArray("messages");
  JsonObject systemMessage = messages.createNestedObject();
  systemMessage["role"] = "system";
  systemMessage["content"] = systemPrompt;
  JsonObject userMessage = messages.createNestedObject();
  userMessage["role"] = "user";
  userMessage["content"] = cleanTranscript;

  String requestBody;
  serializeJson(requestDoc, requestBody);

  lastHttpCode_ = http.POST(requestBody);
  String responseBody = http.getString();
  http.end();

  if (lastHttpCode_ < 200 || lastHttpCode_ >= 300) {
    lastError_ = lastHttpCode_ <= 0
        ? String("Groq request failed: ") + HTTPClient::errorToString(lastHttpCode_)
        : String("Groq API returned HTTP ") + lastHttpCode_;
    DynamicJsonDocument errorDoc(2048);
    if (deserializeJson(errorDoc, responseBody) == DeserializationError::Ok) {
      const char *message = errorDoc["error"]["message"] | "";
      if (message[0] != '\0') {
        lastError_ += ": ";
        lastError_ += message;
      }
      JsonVariant failedGeneration = errorDoc["error"]["failed_generation"];
      if (!failedGeneration.isNull()) {
        String details;
        serializeJson(failedGeneration, details);
        if (details.length() > 0) {
          lastError_ += " | failed_generation: ";
          lastError_ += details.substring(0, 300);
        }
      }
    }
    draft.error = lastError_;
    return false;
  }

  DynamicJsonDocument apiResponse(8192);
  DeserializationError apiError = deserializeJson(apiResponse, responseBody);
  if (apiError) {
    lastError_ = String("Could not parse Groq response JSON: ") + apiError.c_str();
    draft.error = lastError_;
    return false;
  }

  String aiContent = apiResponse["choices"][0]["message"]["content"] | "";
  aiContent.trim();
  if (aiContent.isEmpty()) {
    lastError_ = "Groq returned an empty WhatsApp-draft response.";
    draft.error = lastError_;
    return false;
  }

  DynamicJsonDocument messageDoc(2048);
  DeserializationError messageError = deserializeJson(messageDoc, aiContent);
  if (messageError) {
    lastError_ = String("Could not parse generated WhatsApp draft: ") + messageError.c_str();
    draft.error = lastError_;
    return false;
  }

  if (!messageDoc["send"].is<bool>() ||
      !messageDoc["contact"].is<const char *>() ||
      !messageDoc["text"].is<const char *>()) {
    lastError_ = "Generated WhatsApp draft has an invalid schema.";
    draft.error = lastError_;
    return false;
  }

  draft.send = messageDoc["send"].as<bool>();
  draft.contact = messageDoc["contact"].as<String>();
  draft.text = messageDoc["text"].as<String>();
  draft.contact.trim();
  draft.text.trim();

  if (!draft.send) {
    draft.contact = "";
    draft.number = "";
    draft.text = "";
    return true;
  }

  const int contactIndex = findContact(draft.contact, contacts, contactCount);
  if (contactIndex < 0) {
    draft.send = false;
    draft.contact = "";
    draft.text = "";
    lastError_ = "Groq selected a contact that is not in the allowlist.";
    draft.error = lastError_;
    return false;
  }

  draft.contact = contacts[contactIndex].name;
  if (draft.text.isEmpty()) {
    draft.send = false;
    lastError_ = "Generated WhatsApp message is empty.";
    draft.error = lastError_;
    return false;
  }

  String configuredNumber;
  if (phoneNumbers != nullptr) {
    for (size_t i = 0; i < phoneNumberCount; ++i) {
      if (phoneNumbers[i].name == nullptr || phoneNumbers[i].number == nullptr) continue;
      if (normalizedName(String(phoneNumbers[i].name)) ==
          normalizedName(draft.contact)) {
        configuredNumber = String(phoneNumbers[i].number);
        configuredNumber.trim();
        break;
      }
    }
  }

  bool validPhoneNumber = configuredNumber.startsWith("+");
  String digits = validPhoneNumber ? configuredNumber.substring(1) : String("");
  if (digits.length() < 8 || digits.length() > 15) validPhoneNumber = false;
  for (size_t i = 0; i < digits.length(); ++i) {
    if (!isDigit(digits[i])) {
      validPhoneNumber = false;
      break;
    }
  }

  if (!validPhoneNumber) {
    draft.send = false;
    draft.number = "";
    lastError_ = "Configure a valid international phone number for " + draft.contact + ".";
    draft.error = lastError_;
    return false;
  }

  draft.number = configuredNumber;
  return true;
}

bool GroqReasoning::generateWhatsAppDraft(
    const String &transcript,
    const ContactMapping *contacts,
    size_t contactCount,
    const ContactPhoneMapping *phoneNumbers,
    size_t phoneNumberCount,
    WhatsAppDraft &draft,
    const char *currentDateTime) {
  if (phoneNumberCount == 0) {
    return generateWhatsAppDraft(
        transcript, contacts, contactCount,
        static_cast<const ContactNumberMapping *>(nullptr), 0, draft);
  }
  if (phoneNumbers == nullptr) {
    draft = WhatsAppDraft();
    draft.error = "Phone-number mapping is missing.";
    lastError_ = draft.error;
    return false;
  }

  ContactNumberMapping *converted =
      new (std::nothrow) ContactNumberMapping[phoneNumberCount];
  if (converted == nullptr) {
    draft = WhatsAppDraft();
    draft.error = "Not enough memory to prepare WhatsApp contact mappings.";
    lastError_ = draft.error;
    return false;
  }

  for (size_t i = 0; i < phoneNumberCount; ++i) {
    converted[i].name = phoneNumbers[i].name;
    converted[i].number = phoneNumbers[i].number;
  }

  const bool ok = generateWhatsAppDraft(
      transcript, contacts, contactCount, converted, phoneNumberCount, draft, currentDateTime);
  delete[] converted;
  return ok;
}

bool GroqReasoning::sendWhatsAppMessage(
    WhatsAppDraft &draft,
    const WhatsAppConfig &config) {
  draft.submitted = false;
  draft.apiStatus = "";
  lastError_ = "";
  lastHttpCode_ = 0;

  if (!draft.send || draft.number.isEmpty() || draft.text.isEmpty()) {
    lastError_ = "WhatsApp draft is not ready to send.";
    draft.error = lastError_;
    return false;
  }
  if (config.apiUrl == nullptr || config.apiUrl[0] == '\0' ||
      config.instance == nullptr || config.instance[0] == '\0' ||
      config.apiKey == nullptr || config.apiKey[0] == '\0') {
    lastError_ = "Evolution API URL, instance, or API key is missing.";
    draft.error = lastError_;
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    lastError_ = "Wi-Fi is not connected.";
    draft.error = lastError_;
    return false;
  }

  String baseUrl(config.apiUrl);
  baseUrl.trim();
  while (baseUrl.endsWith("/")) baseUrl.remove(baseUrl.length() - 1);
  if (!baseUrl.startsWith("https://") && !baseUrl.startsWith("http://")) {
    lastError_ = "Evolution API URL must start with https:// or http://.";
    draft.error = lastError_;
    return false;
  }

  String instance(config.instance);
  instance.trim();
  String encodedInstance;
  for (size_t i = 0; i < instance.length(); ++i) {
    const char ch = instance[i];
    if (isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') {
      encodedInstance += ch;
    } else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", static_cast<unsigned char>(ch));
      encodedInstance += hex;
    }
  }

  String number;
  for (size_t i = 0; i < draft.number.length(); ++i) {
    const char ch = draft.number[i];
    if (isDigit(ch)) number += ch;
  }
  if (number.length() < 8 || number.length() > 15) {
    lastError_ = "WhatsApp recipient number is invalid.";
    draft.error = lastError_;
    return false;
  }

  WiFiClientSecure tls;
  tls.setInsecure();

  HTTPClient http;
  http.setConnectTimeout(CONNECT_TIMEOUT_MS);
  http.setTimeout(REQUEST_TIMEOUT_MS);

  const String endpoint =
      baseUrl + "/message/sendText/" + encodedInstance;
  if (!http.begin(tls, endpoint)) {
    lastError_ = "Could not initialize Evolution API request.";
    draft.error = lastError_;
    return false;
  }

  http.addHeader("apikey", config.apiKey);
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument requestDoc(1024 + draft.text.length() * 2);
  requestDoc["number"] = number;
  requestDoc["text"] = draft.text;
  String requestBody;
  serializeJson(requestDoc, requestBody);

  lastHttpCode_ = http.POST(requestBody);
  String responseBody = http.getString();
  http.end();

  if (lastHttpCode_ < 200 || lastHttpCode_ >= 300) {
    lastError_ = lastHttpCode_ <= 0
        ? String("Evolution API request failed: ") + HTTPClient::errorToString(lastHttpCode_)
        : String("Evolution API returned HTTP ") + lastHttpCode_;
    if (responseBody.length()) {
      lastError_ += " ";
      lastError_ += responseBody.substring(0, 400);
    }
    draft.apiStatus = String(lastHttpCode_);
    draft.error = lastError_;
    return false;
  }

  draft.apiStatus = String(lastHttpCode_);
  draft.submitted = true;
  draft.error = "";
  return true;
}

bool GroqReasoning::sendWhatsAppMessage(
    WhatsAppDraft &draft,
    const char *apiBaseUrl,
    const char *instanceName,
    const char *apiKey) {
  const WhatsAppConfig config = {apiBaseUrl, instanceName, apiKey};
  return sendWhatsAppMessage(draft, config);
}


namespace {
enum RuntimeScheduleState {
  RUNTIME_NO_SCHEDULE,
  RUNTIME_SCHEDULE_READY,
  RUNTIME_SCHEDULE_CLARIFY,
  RUNTIME_SCHEDULE_INVALID
};

bool runtimeClockReady() {
  return time(nullptr) > 1700000000;
}

String runtimeTimeLabel(time_t value) {
  struct tm tmValue;
  if (localtime_r(&value, &tmValue) == nullptr) return "an unknown time";
  char buffer[56];
  strftime(buffer, sizeof(buffer), "%I:%M %p on %d %B", &tmValue);
  return String(buffer) + " IST";
}

String runtimeDeliveryPrefix(String value) {
  value.toLowerCase();
  value.replace(".", "");
  const char *markers[] = {
    " saying ", " say ", " that says ", " that ", " message says ",
    " with the message ", " with text ", " text saying "
  };
  int cut = -1;
  for (size_t i = 0; i < sizeof(markers) / sizeof(markers[0]); ++i) {
    int p = value.indexOf(markers[i]);
    if (p >= 0 && (cut < 0 || p < cut)) cut = p;
  }
  if (cut >= 0) value.remove(cut);
  value.trim();
  return value;
}

bool runtimeFindClock(const String &value, int &timeStart) {
  int best = -1;
  int bestStart = -1;
  const char *markers[] = {" at ", " for "};
  for (size_t i = 0; i < 2; ++i) {
    int from = 0;
    while (true) {
      int p = value.indexOf(markers[i], from);
      if (p < 0) break;
      int start = p + strlen(markers[i]);
      while (start < value.length() && value[start] == ' ') ++start;
      if (start < value.length() && isDigit(value[start])) {
        if (best < 0 || p < best) { best = p; bestStart = start; }
        break;
      }
      from = p + strlen(markers[i]);
    }
  }
  if (bestStart < 0) return false;
  timeStart = bestStart;
  return true;
}

RuntimeScheduleState runtimeParseSchedule(
    const String &transcript, time_t &target, String &response) {
  const String prefix = runtimeDeliveryPrefix(transcript);
  const bool tomorrow = prefix.indexOf("tomorrow") >= 0;
  const bool today = prefix.indexOf("today") >= 0;
  const bool later = prefix.indexOf("later") >= 0 ||
                     prefix.indexOf("schedule") >= 0 ||
                     prefix.indexOf("scheduled") >= 0;
  int start = -1;
  const bool hasClock = runtimeFindClock(prefix, start);
  if (!hasClock && !tomorrow && !today && !later) return RUNTIME_NO_SCHEDULE;
  if (!hasClock) {
    response = "What time should I send it? Say, for example, tomorrow at 8 PM.";
    return RUNTIME_SCHEDULE_CLARIFY;
  }
  if (!runtimeClockReady()) {
    response = "I need an internet time sync before scheduling. Check Wi-Fi and try again.";
    return RUNTIME_SCHEDULE_CLARIFY;
  }

  String clock = prefix.substring(start);
  clock.trim();
  if (clock.startsWith("about ")) clock = clock.substring(6);
  else if (clock.startsWith("around ")) clock = clock.substring(7);

  int i = 0;
  while (i < clock.length() && isDigit(clock[i])) ++i;
  if (i == 0) {
    response = "Please repeat the time, for example 8:30 PM.";
    return RUNTIME_SCHEDULE_INVALID;
  }

  String digits = clock.substring(0, i);
  int hour = 0, minute = 0;
  bool colon = false;
  if (i < clock.length() && clock[i] == ':') {
    colon = true;
    hour = digits.toInt();
    ++i;
    int m = i;
    while (i < clock.length() && isDigit(clock[i])) ++i;
    if (m == i) {
      response = "I couldn't read the minutes. Please repeat the time.";
      return RUNTIME_SCHEDULE_INVALID;
    }
    minute = clock.substring(m, i).toInt();
  } else if (digits.length() >= 3) {
    colon = true;
    hour = digits.substring(0, digits.length() - 2).toInt();
    minute = digits.substring(digits.length() - 2).toInt();
  } else {
    hour = digits.toInt();
  }

  String suffix = clock.substring(i);
  suffix.trim();
  const bool am = suffix.startsWith("am") || suffix.startsWith("a m");
  const bool pm = suffix.startsWith("pm") || suffix.startsWith("p m");
  if (minute > 59 || hour > 23) {
    response = "That time is invalid. Please say a valid time.";
    return RUNTIME_SCHEDULE_INVALID;
  }
  if (am || pm) {
    if (hour < 1 || hour > 12) {
      response = "Please repeat the time using a valid AM or PM value.";
      return RUNTIME_SCHEDULE_INVALID;
    }
    hour %= 12;
    if (pm) hour += 12;
  } else if (hour <= 12) {
    response = "Should I use AM or PM? Please repeat the time with AM or PM.";
    return RUNTIME_SCHEDULE_CLARIFY;
  }

  const time_t now = time(nullptr);
  struct tm local;
  if (localtime_r(&now, &local) == nullptr) {
    response = "I couldn't read the Indian clock.";
    return RUNTIME_SCHEDULE_INVALID;
  }
  local.tm_hour = hour;
  local.tm_min = minute;
  local.tm_sec = 0;
  if (tomorrow) local.tm_mday += 1;
  target = mktime(&local);
  if (target <= now && !tomorrow) {
    if (today) {
      response = "That time has already passed today. Please give a future time or say tomorrow.";
      return RUNTIME_SCHEDULE_CLARIFY;
    }
    local.tm_mday += 1;
    target = mktime(&local);
  }
  response = "";
  (void)colon;
  return RUNTIME_SCHEDULE_READY;
}
} // namespace

bool GroqReasoning::syncIndianClock(bool waitForSync) {
  if (WiFi.status() != WL_CONNECTED) return false;
  configTzTime("IST-5:30", "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  lastNtpAttemptMs_ = millis();
  if (!waitForSync) return runtimeClockReady();
  const unsigned long started = millis();
  while (millis() - started < 10000) {
    if (runtimeClockReady()) return true;
    delay(200);
  }
  return runtimeClockReady();
}

String GroqReasoning::currentIndianTimeContext() const {
  if (!runtimeClockReady()) return "";
  const time_t now = time(nullptr);
  struct tm local;
  if (localtime_r(&now, &local) == nullptr) return "";
  char buffer[80];
  strftime(buffer, sizeof(buffer), "%A, %Y-%m-%d %H:%M:%S IST (UTC+05:30)", &local);
  return String(buffer);
}

bool GroqReasoning::generateWhatsAppDraft(
    const String &transcript,
    const ContactMapping *contacts,
    size_t contactCount,
    WhatsAppDraft &draft,
    const char *currentDateTime) {
  if (!contacts || !contactCount) {
    draft = WhatsAppDraft();
    draft.error = "No WhatsApp contacts are configured.";
    lastError_ = draft.error;
    return false;
  }
  ContactNumberMapping *numbers =
      new (std::nothrow) ContactNumberMapping[contactCount];
  if (!numbers) {
    draft = WhatsAppDraft();
    draft.error = "Not enough memory for contact mappings.";
    lastError_ = draft.error;
    return false;
  }
  for (size_t i = 0; i < contactCount; ++i) {
    numbers[i].name = contacts[i].name;
    numbers[i].number = contacts[i].number;
  }
  const bool ok = generateWhatsAppDraft(
      transcript, contacts, contactCount, numbers, contactCount, draft, currentDateTime);
  delete[] numbers;
  return ok;
}

GroqReasoning::Result GroqReasoning::processTranscript(
    const String &transcript,
    const DeviceMapping *devices,
    size_t deviceCount,
    const ContactMapping *contacts,
    size_t contactCount,
    const WhatsAppConfig &whatsappConfig) {
  Result result;
  result.response = "Sorry, I couldn't process that request right now.";
  result.action = "none";
  String clean = transcript;
  clean.trim();
  if (clean.isEmpty()) {
    result.response = "Sorry, I couldn't understand that.";
    return result;
  }

  serviceScheduledMessages(whatsappConfig);
  const String now = currentIndianTimeContext();

  if (!looksLikeWhatsAppRequest(clean, contacts, contactCount)) {
    return ask(clean, devices, deviceCount, contacts, contactCount, now.c_str());
  }

  time_t deliveryEpoch = 0;
  String scheduleResponse;
  const RuntimeScheduleState schedule = runtimeParseSchedule(clean, deliveryEpoch, scheduleResponse);
  if (schedule == RUNTIME_SCHEDULE_CLARIFY || schedule == RUNTIME_SCHEDULE_INVALID) {
    result.response = scheduleResponse;
    return result;
  }
  if (schedule == RUNTIME_SCHEDULE_READY && scheduledMessagePending_) {
    result.response = "A WhatsApp message is already scheduled for " +
                      runtimeTimeLabel(scheduledMessageEpoch_) +
                      ". Send it or cancel it before scheduling another.";
    return result;
  }

  WhatsAppDraft draft;
  if (!generateWhatsAppDraft(clean, contacts, contactCount, draft, now.c_str())) {
    result.response = "I couldn't prepare the WhatsApp message: " + draft.error;
    return result;
  }
  if (!draft.send) {
    result.response = "What would you like me to say in the message?";
    return result;
  }

  if (schedule == RUNTIME_SCHEDULE_READY) {
    Preferences prefs;
    if (!prefs.begin("wa_sched", false)) {
      result.response = "I couldn't save the scheduled message.";
      return result;
    }
    prefs.putULong64("epoch", static_cast<uint64_t>(deliveryEpoch));
    const bool stored =
        prefs.putString("contact", draft.contact) > 0 &&
        prefs.putString("number", draft.number) > 0 &&
        prefs.putString("text", draft.text) > 0;
    prefs.putBool("pending", stored);
    prefs.end();
    if (!stored) {
      result.response = "I couldn't save the scheduled message.";
      return result;
    }
    scheduledMessagePending_ = true;
    scheduledMessageEpoch_ = deliveryEpoch;
    scheduledMessageContact_ = draft.contact;
    scheduledMessageNumber_ = draft.number;
    scheduledMessageText_ = draft.text;
    draft.scheduled = true;
    draft.scheduledFor = runtimeTimeLabel(deliveryEpoch);
    result.response = "Scheduled the WhatsApp message to " + draft.contact +
                      " for " + draft.scheduledFor + ".";
    return result;
  }

  if (sendWhatsAppMessage(draft, whatsappConfig)) {
    result.response = "WhatsApp message submitted to Evolution API for " + draft.contact + ".";
  } else {
    result.response = "WhatsApp message could not be submitted: " + draft.error;
  }
  return result;
}

void GroqReasoning::serviceScheduledMessages(const WhatsAppConfig &whatsappConfig) {
  if (!scheduleLoaded_) {
    scheduleLoaded_ = true;
    Preferences prefs;
    if (prefs.begin("wa_sched", true)) {
      scheduledMessagePending_ = prefs.getBool("pending", false);
      if (scheduledMessagePending_) {
        scheduledMessageEpoch_ = static_cast<time_t>(prefs.getULong64("epoch", 0));
        scheduledMessageContact_ = prefs.getString("contact", "");
        scheduledMessageNumber_ = prefs.getString("number", "");
        scheduledMessageText_ = prefs.getString("text", "");
        if (scheduledMessageEpoch_ <= 0 || scheduledMessageContact_.isEmpty() ||
            scheduledMessageNumber_.isEmpty() || scheduledMessageText_.isEmpty()) {
          scheduledMessagePending_ = false;
        }
      }
      prefs.end();
    }
  }

  if (!runtimeClockReady() && WiFi.status() == WL_CONNECTED &&
      (lastNtpAttemptMs_ == 0 || millis() - lastNtpAttemptMs_ >= 60000)) {
    syncIndianClock(false);
  }
  if (!scheduledMessagePending_ || !runtimeClockReady() ||
      time(nullptr) < scheduledMessageEpoch_ ||
      WiFi.status() != WL_CONNECTED || !initialized_) return;

  const unsigned long nowMs = millis();
  if (lastScheduledSendAttemptMs_ &&
      nowMs - lastScheduledSendAttemptMs_ < 60000) return;
  lastScheduledSendAttemptMs_ = nowMs;

  WhatsAppDraft draft;
  draft.send = true;
  draft.contact = scheduledMessageContact_;
  draft.number = scheduledMessageNumber_;
  draft.text = scheduledMessageText_;
  if (!sendWhatsAppMessage(draft, whatsappConfig)) return;

  Preferences prefs;
  if (prefs.begin("wa_sched", false)) {
    prefs.putBool("pending", false);
    prefs.remove("epoch");
    prefs.remove("contact");
    prefs.remove("number");
    prefs.remove("text");
    prefs.end();
  }
  scheduledMessagePending_ = false;
  scheduledMessageEpoch_ = 0;
  scheduledMessageContact_ = "";
  scheduledMessageNumber_ = "";
  scheduledMessageText_ = "";
}
