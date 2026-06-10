# app/events/

**Shape:** Event — reserved app-level typed event definitions.

No app-level event sources are active here yet. The shipped event machinery
lives in `lib/event/` and the durable reducer fact log lives in `lib/storage/`.
Keep this directory empty until an event file has a concrete typed contract and
subscriber surface that belongs to the app layer.

Do not add placeholder event files or macro-only scaffold. When this shape is
filled in, each file should declare one event family with a stable payload
contract and a clear producer/consumer boundary.

See [`docs/FRAMEWORK.md`](../../docs/FRAMEWORK.md) for the target Event shape.
