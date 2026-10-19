# PatientController 1.0.0

Serial JSON request/acknowledgement client between the main ESP32-S3 and secondary ESP32.

The library sends a unique request ID and waits for a matching acknowledgement. The secondary ESP32 firmware must implement the same protocol and return the matching ID, success, status, and message fields.

Defaults: RX GPIO44, TX GPIO43, 115200 baud, 1800 ms acknowledgement timeout.

Dependency: ArduinoJson 6.x.
