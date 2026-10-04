# Diagnostic text logger

`import mddlog.adapter.textlogger;` provides `mddlog::adapter::TextLogger` in the
`mddlog::mddlog` target. Its atomic per-group flags start disabled; `set()` and `is()`
operate independently, so Warn can be disabled while Error remains enabled. Trace shares
Debug's mask and renders as `D`; Fatal shares Error's mask and renders as `E`. Invalid
severity values are disabled. `disableAll()` masks every group.
Atomicity applies to each group separately. `disableAll()` and any caller that
changes several groups update them in sequence, with no atomic transition across groups. Concurrent `is()` or
write calls may therefore observe a partially applied change.

`write(level, text)` renders `[X] HH:MM:SS | text`. Passing a `source_location` adds
`file:line |` between the time and message, with the basename padded to 16 columns and
the line padded to 4. The timestamp uses `system_clock` and retains the standard library's
fractional-second precision. `writeDump()` emits a rendered message followed by a separate,
unprefixed dump, under one enablement decision. Rendering and callbacks are synchronous and
allocating; browser transport buffering and failure handling remain separate (#68/#70).

Callbacks are registered through #67's `SinkRegistry<void(string_view)>`. Each callback has
its own handle. Removal outside a callback waits for quiescence; self-removal and detected
mutual cross-removal follow the registry's deferred-removal contract. Handles are explicit
registrations, not RAII subscriptions: retain them and remove them before destroying captured
objects. A callback must copy any text it retains after returning.

## Application facades

An application that keeps its own logging API (a level threshold, a hex dump, its own call
context) maps it onto `TextLogger` in its own code: mddlog ships no application facade and no
header-only surface. The application owns that mapping and verifies it in its own test suite
against a pinned mddlog release; mddlog's tests contain no application code
([ADR-003, Amendment 1](../adr/ADR-003-application-integration-and-sink-ownership.md#amendment-1-verification-boundary-103)).
