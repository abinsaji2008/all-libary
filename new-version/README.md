# Smart Patient Assistant — New Version

**Status:** Planning scaffold only. No existing library source has been changed.

This folder is reserved for the new Smart Patient Assistant version. The existing `libraries/` directory remains untouched while the workflow is being agreed.

## Confirmed design requirements

- Call `GroqReasoning.ask()` directly from the main sketch; do not add a `Reasoning_Command()` wrapper.
- Expand the reasoning result to support these fields: `Response`, `Action`, `Name`, `Message`, and `History`.
- Handle the current date/time inside the new version of the `GroqReasoning` library, rather than requiring the main sketch to build the timestamp.
- Keep contact phone numbers in device-side code/configuration; do not send phone numbers to the AI service.
- Execute the requested action and capture its real outcome before speaking the final response with `GroqTTS`.
- Do not add WhatsApp message scheduling.
- Preserve the existing library source until the new workflow is approved and implemented separately.

## Planned layout

The exact layout and APIs will be finalized before implementation. The likely components are:

- Main ESP32-S3 sketch: pins, library setup, workflow callbacks, prompts/context, result routing, action execution, response speech, and task history.
- New-version `GroqReasoning`: system/user prompt handling, internal time handling, structured result fields, and reasoning/API error handling.
- Reuse or adapt the existing audio-stream, TTS, wake-word, LED, WhatsApp, and patient-controller libraries only after their interfaces are checked.

## Preliminary line-count estimate

These are estimates, not limits:

- Main sketch: approximately **300–450 lines**.
- New-version GroqReasoning code (header and implementation): approximately **350–550 lines**.
- Planning/configuration glue and documentation: approximately **50–100 lines**.
- Expected new code total: approximately **700–1,100 lines**, excluding reused libraries.

The final count depends on the approved task-history format, JSON schema, error handling, and which existing library interfaces can be reused unchanged.

## Not implemented yet

No firmware, library implementation, API changes, or hardware tests have been performed as part of this planning scaffold.
