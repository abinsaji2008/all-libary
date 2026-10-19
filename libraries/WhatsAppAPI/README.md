# WhatsAppAPI 1.0.0

Separate Evolution API transport for WhatsApp text-message submissions.

- `begin(baseUrl, instanceName, apiKey)` configures the API endpoint.
- `sendText(phoneNumber, text)` validates inputs and submits a message.
- A successful 2xx response means the API accepted the submission; it does not prove delivery.
- Phone numbers are normalised locally and are not sent to Groq.

Dependency: ArduinoJson 6.x. This development build uses insecure TLS; configure certificate validation before production.
