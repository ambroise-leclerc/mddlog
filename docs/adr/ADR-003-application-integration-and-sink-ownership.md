# ADR-003: Application integration and sink ownership

## Status
Proposed — drafted for maintainer review, not yet acted on.

Requested in the review of PR #6: ADR-001 and ADR-002 define a bounded core and an audit contract,
neither of which is sufficient to replace an existing application logger. This record covers the
integration boundary, using [WebFront](https://github.com/ambroise-leclerc/WebFront) as the concrete
consumer because it is the one that exists.

**Baseline read for this record**: the local WebFront checkout at
`7be626ccfbb50524c9c03296e6c84555ea7d2c7c`. The PR review cited `d927b05a…`; the working copy has
moved since, so every claim below names the file and line it was read from at `7be626c` rather than
relying on the earlier description. Re-verify against whichever commit is authoritative before this
record moves to Accepted.

## Context

### What WebFront's logger is today

`webfront::log` (`include/tooling/Logger.hpp`, 71 lines) is header-only and pre-modules:

- **Levels are `const uint8_t` constants**, not an enum class: `webfront::log::Disabled = 0, Error = 1,
  Warn = 2, Info = 3, Debug = 4` (line 18). The numeric order is **inverted** relative to mddlog's
  `LogLevel` (`LogLevel::Trace = 0 … LogLevel::Audit = 6`, where higher means more severe). Both sides
  now spell `Error`, `Warn`, `Info` and `Debug` identically, with different numeric values, so the
  qualifier (`webfront::log::` or `LogLevel::`) is what tells them apart below. Any mapping must be written out rather
  than assumed to be a cast.
- **Enablement is per level, not a threshold**: `inline bool logTypeEnabled[Debug + 1]` (line 20)
  with `set()`, `is()` and `setLogLevel()` (lines 55-57). `test/LoggerTests.cpp:37-69` pins this
  precisely — it disables `Warn` and `Error` while leaving `Info` and `Debug` on, then inverts the
  combination, and asserts the resulting level characters in order (`{'I','D','W','E','W','I','E'}`).
  mddlog's single `minLevel_` threshold **cannot express that**; this is a capability difference, not
  a naming difference.
- **Formatting is eager, at the call site**: `std::format`/`vformat` produce a `std::string` which is
  passed to every sink synchronously (lines 36-53). The rendered shape is
  `[X] HH:MM:SS | file:line | text`, and the timestamp is `{:%T}` of `system_clock::now()` — **time
  of day only, no date**.
- **`source_location` is carried by `debug` only** (lines 60-64, via the struct-plus-deduction-guide
  trick that allows a trailing defaulted argument after a parameter pack). `error`, `warn` and `info`
  (lines 65-67) have none. `LoggerTests.cpp:31-33` asserts the filename appears in a debug line, and
  skips that assertion on llvm.
- **`infoHex`** (line 68) emits a binary dump through `utils::hexDump` (`HexDump.hpp:68`) as a second
  sink write following the message.

### What its sink registry does, and where it breaks

`Sinks` holds `inline static std::vector<std::function<void(std::string_view)>> sinks` (line 26) with
**no synchronization**, and three defects follow directly from that and from the registration
lifecycle in `WebLink`:

1. **Registration races with emission.** `addSinks` does `push_back` (line 69), which may reallocate,
   while `Sinks::operator()` iterates `for (auto& s : sinks)` (lines 22-25). A browser connection
   completing its handshake on one thread while another thread logs is a data race and a potential
   iterator invalidation.
2. **Removal does not wait for in-flight invocations.** `removeSinks` assigns `sinks[id] = nullptr`
   (line 70), and `~WebLink` calls it (`WebLink.hpp:77`) — but a concurrent `operator()` may already
   hold a reference to that `std::function` and be inside a callback that captures the `WebLink`
   `this` (`WebLink.hpp:120`). That is precisely the "callback bound to a destroyed connection is
   still invoked" case. Nulling the slot also never erases it, so the vector grows monotonically for
   the process lifetime, one slot per connection ever made.
3. **`addSinks` returns only the last id.** It is a fold over `push_back` followed by
   `return out.sinks.size() - 1` (line 69), so registering several sinks in one call makes every id
   but the last unrecoverable, and therefore unremovable.

### Why the browser sink cannot use a naive path

The sink registered at handshake sends the log line over the same WebSocket it is logging about:
`logSink = log::addSinks([this](std::string_view t) { sendCommand(msg::TextCommand(msg::TxtOpcode::debugLog, t)); });`
(`WebLink.hpp:120`). `sendCommand` calls `ws->write` (line 80). `WebLink` logs on many of the paths
that reach a write — construction (line 58), every binary message via `log::infoHex` (line 103),
`log::error` on an empty message (line 105), `log::info` in `handleCallFunction` (lines 132, 142).
So a log emitted while sending a log re-enters the sink. Today nothing breaks that cycle; a transport
that starts failing and logging its failures is the amplification case the review names.

### What correlation already exists

WebFront already has the identifiers ADR-001's emission-time context envelope needs:
`WebLinkId = uint16_t` (`WebLink.hpp:23`), `msg::CallId` allocated per link from `nextCallId{1}`
(line 52), `WebLinkEvent` carrying both (lines 28-34), and `expectResult()` returning
`{CallId, std::future<Result>}` with a `pendingCalls` map under `pendingMutex` (lines 84-99). The
integration does not need to invent correlation; it needs to stop discarding it into a pre-formatted
string.

### The consumption gap

WebFront is header-only, `cmake_minimum_required(VERSION 3.31)`, `cxx_std_23`
(`CMakeLists.txt:1, 33`). mddlog requires CMake 4.0, C++23 **modules** and `import std`
(`CMakeLists.txt:1-7, 29-43`). Both using C++23 does not make one consumable by the other, and this
record must not imply otherwise.

## Medical Device Considerations

This record is mostly not a medical-device decision: WebFront is a general-purpose UI library, and
forcing an audit model onto it would be the wrong direction. Two points do matter:

- **The audit lane stays optional and separate.** ADR-002's `AuditEvent` path must not be enabled by
  default for a consumer like this, and audit events must never be routed to a browser sink as
  ordinary diagnostics — a regulatory record streamed to a web client as debug text is neither
  protected nor bounded.
- **Diagnostics are not evidence.** Anything this integration streams is a diagnostic aid. Nothing in
  it supports a traceability or audit claim, and the facade should make that hard to confuse.

## Decision

### 1. A facade, with the existing test suite as the contract

Provide a `webfront::log`-shaped facade over mddlog rather than asking call sites to change:
`debug`/`info`/`warn`/`error`/`infoHex`, `set`/`is`/`setLogLevel`, `addSinks`/`removeSinks`. The
behaviors pinned by `test/LoggerTests.cpp` are the acceptance criteria — the level character in
position 1, the message at the end of the line, the filename in a debug line, and the exact
enable/disable sequence of lines 37-69. A migration that changes the rendered shape breaks that
suite, and the suite is right to break.

### 2. Per-level enablement, not a threshold

mddlog's `minLevel_` cannot express "Warn off, Error on". The adapter therefore carries a per-level
enable mask and maps it explicitly:

| WebFront | mddlog |
|---|---|
| `webfront::log::Error` (1) | `LogLevel::Error` (4) |
| `webfront::log::Warn` (2) | `LogLevel::Warn` (3) |
| `webfront::log::Info` (3) | `LogLevel::Info` (2) |
| `webfront::log::Debug` (4) | `LogLevel::Debug` (1) |
| `webfront::log::Disabled` (0) | all levels masked off |

`LogLevel::Trace`, `LogLevel::Fatal` and `LogLevel::Audit` have no WebFront equivalent: `Trace` maps
into `webfront::log::Debug` for display, `Fatal` into `webfront::log::Error`, and `Audit` is **not routed to this facade at all** (Decision 6). Whether
mddlog's own logger should gain a per-level mask, rather than leaving it in the adapter, is an open
question this record deliberately leaves to the implementing issue — the adapter can carry it either
way.

### 3. Consumption without modules is an explicit choice, not an implicit promise

Three options, and this record recommends the second:

1. **Isolated adapter translation unit** — one TU consumes mddlog's modules and exposes a plain
   header to WebFront. WebFront keeps CMake 3.31 and header-only usage; the adapter is the only thing
   needing CMake 4, modules and `import std`.
2. **Optional dependency** — same adapter, built only when a consumer asks for it, so WebFront's
   default build is unchanged and mddlog's toolchain floor is not imposed on anyone who does not opt
   in. *Recommended*: it is option 1 plus an honest default.
3. **Not supported** — WebFront keeps its own logger. A legitimate outcome, and better than a shim
   nobody can build.

What this record rules out is a fourth, unstated option: implying that "both are C++23" makes the
dependency work. It does not, and the CMake floors differ by a major version.

### 4. Sink ownership: handles with removal that waits

Replace the global unsynchronized `std::vector<std::function<...>>` on the mddlog side of the
boundary with a registry where:

- registration and emission do not race (emission takes a stable snapshot; registration does not
  reallocate under an in-flight iteration);
- **removal waits for in-flight invocations of that sink to finish before returning**, so a
  `~WebLink` that has returned guarantees its captured `this` is no longer reachable from any sink
  call. This is the property `sinks[id] = nullptr` does not provide;
- slots are reclaimed rather than leaked one per connection;
- registering several sinks returns one handle **per sink**, fixing the `size() - 1` defect.

**Self-removal must be defined before quiescent removal is safe.** A sink callback that removes its
own handle would wait for an invocation that cannot finish until the wait returns — a deadlock
introduced by the very guarantee above. The rule adopted here is **deferred self-removal**: a
removal issued from inside the sink being removed returns immediately, marks the handle as retiring
so no new invocation starts, and completes once the current invocation returns. Removal from any
other context waits as described. The two remaining options — rejecting the self-call, or a
non-waiting self path with no completion guarantee — are worse: the first makes a sink unable to
retire itself in response to its own transport error, which Decision 5 needs; the second gives back
the guarantee the decision exists for. A removal issued from inside a *different* sink's callback
still waits, and an implementation must not let two sinks removing each other wait in a cycle.

**The handle change is a breaking change for multi-sink call sites, deliberately.** Today
`addSinks(a, b, c)` returns one `size_t`; the replacement returns one handle per sink, so a call
site registering several sinks at once must bind several handles and pass them individually to
`removeSinks`. Keeping the function names does not keep those call sites compiling. This record
chooses the break rather than a compatibility wrapper, because the single-id return is not a
convenience to preserve — it is the defect that makes every sink but the last unremovable. For
single-sink registration, which is what `WebLink.hpp:120` and both example programs actually do, the
shape is unchanged. A variadic call returning a tuple or array of handles is the recommended
spelling; the implementing issue picks it.

### 5. The transport consumer is bounded, non-reentrant, and fails quietly

For a sink that writes to a transport (the browser sink being the motivating case):

- **Bounded**: it consumes from a bounded ring (ADR-001 Decision 4), not from the emitting thread. A
  slow or stalled browser must not block a producer, and saturation refuses with an observable
  counter rather than growing without bound.
- **Non-reentrant**: logs emitted while dispatching to a sink must not re-enter that sink. A
  thread-local "in dispatch" guard (or a dedicated consumer thread that never logs through the
  registry) breaks the `sendCommand` → `ws->write` → `log::…` → `sendCommand` cycle documented in
  Context.
- **Fails quietly and locally**: a transport error detaches the sink and is reported through the
  registry's own health counters, not by logging the failure through the path that just failed.
- **Disconnection is ordinary**: sink removal at `~WebLink` follows Decision 4, so a disconnect in
  flight is a wait, not a race.

### 6. Context is captured at the producer; formatting happens in the adapter; audit stays out

Following ADR-001 Decision 1, the facade passes `WebLinkId`, `CallId` and a component identifier
through as *fields*, captured when the call is made — not folded into a pre-formatted string as
today. The adapter renders the final text, including the existing `[X] HH:MM:SS | file:line | text`
shape, so Decision 1's test contract still holds while the identifiers survive to any other sink.

`AuditEvent` (ADR-002) is not reachable through this facade. A consumer that wants an audit lane opts
into it explicitly, and it does not share the browser sink.

**Worked example — one call's request, response and error, across two simultaneous connections.**
The identifier that matters is the **pair**, not the `CallId`: `WebLinkId` is allocated per
connection (`WebFront.hpp:347-348`) while `CallId` restarts from `nextCallId{1}` inside each
`WebLink` (`WebLink.hpp:52`), so call 1 on link 1 and call 1 on link 2 are unrelated calls that today
render as indistinguishable text.

Two browsers are connected as links 1 and 2. Each invokes a JS function; `expectResult()` allocates
a `CallId` per link and returns a future (`WebLink.hpp:84-99`), `JsFunction` stamps it on the outgoing
command (`JsFunction.hpp:41`), the reply arrives as `functionReturn` and settles through
`completePending` (`WebLink.hpp:112`), and a failure arrives instead as an encoded exception or an
error (`sendException`/`sendError`, lines 139-153) — or, if the browser disconnects mid-call, as
`rejectPending` from the close handler (line 64):

| Event | component | webLinkId | callId |
|---|---|---|---|
| request sent to browser A | `jsFunction` | 1 | 1 |
| request sent to browser B | `jsFunction` | 2 | 1 |
| response settles for B | `jsFunction` | 2 | 1 |
| error returned for A | `jsFunction` | 1 | 1 |
| A disconnects, pending rejected | `weblink` | 1 | 1 |

Interleaved arbitrarily in one stream, those five lines are today five strings whose only relation is
whatever the call site happened to interpolate. With the identifiers carried as fields, a reader
filters on `(webLinkId, callId)` and gets one call's life without parsing text — and the last two
rows, which belong to the same call but are emitted from different components, stay joined.

Capturing them at emission rather than at drain is what makes this work: by the time a bounded
consumer drains the ring, link 1 may already be destroyed (`~WebLink`, line 74), so there is nothing
left to ask.

## Alternatives Considered

### 1. WebFront depends on mddlog directly (Rejected)
**Pros:** No adapter layer; one logger.
**Cons:** Imposes CMake 4, C++23 modules and `import std` on a header-only CMake 3.31 consumer, and
makes WebFront's toolchain floor a function of mddlog's. Decision 3 option 1/2 gets the same
functionality without that.

### 2. mddlog ships a header-only shim (Rejected as a default)
**Pros:** Simplest possible consumption story.
**Cons:** mddlog's README requires a "Pure C++23 modules library — no legacy headers"; a shipped
header-only surface contradicts that and would have to be maintained as a second public API. An
isolated adapter TU on the consumer side keeps that boundary intact.

### 3. Keep WebFront's logger, integrate nothing (Not rejected — the baseline)
This remains a reasonable outcome. The current logger works for its purpose; its defects (Context,
items 1-3) are fixable in place without adopting mddlog at all. This record exists so that *if*
integration happens, the ownership and re-entrancy questions are answered first — not to argue that
it must.

## Consequences

### Positive
- Names three concrete, reproducible defects in the current registry (reallocation race, removal
  without quiescence, `size() - 1`) with file and line references, so they can be fixed regardless of
  whether integration proceeds.
- Documents the re-entrancy cycle as an existing code path rather than a hypothetical.
- Keeps mddlog's modules-only public surface intact while giving a pre-modules consumer a supported
  path.

### Negative
- A facade plus an adapter TU is a second public surface to maintain, and it exists only for
  consumers that cannot take modules.
- Per-level masking (Decision 2) is capability the mddlog core does not have today; wherever it
  lands, it is new code with its own tests.
- Removal-waits-for-quiescence (Decision 4) is more expensive than nulling a slot, and needs care not
  to deadlock when a sink is removed from inside a sink callback.

### Risks and Mitigations
- **The baseline moved.** This record was read at WebFront `7be626c`, not the `d927b05` the review
  cited. *Mitigation*: every claim names file and line; re-verify before Accepted.
- **The facade freezes a rendered text shape.** `LoggerTests.cpp` asserts on substrings of the
  formatted line, so the adapter inherits a format contract it did not choose. *Mitigation*: treat
  the shape as versioned by that test, and change it only by changing the test deliberately.
- **A fix for re-entrancy hides a real transport failure.** Suppressing logs during dispatch means a
  failing transport reports less, not more. *Mitigation*: health counters in the registry (Decision
  5) are the reporting channel, and they are not routed through sinks.

## References
- WebFront at `7be626ccfbb50524c9c03296e6c84555ea7d2c7c`: `include/tooling/Logger.hpp`,
  `include/weblink/WebLink.hpp`, `test/LoggerTests.cpp`, `include/tooling/HexDump.hpp`,
  `CMakeLists.txt`.
- ADR-001 (this repository) — the bounded ring Decision 5 consumes, and the emission-time context
  envelope Decision 6 relies on.
- ADR-002 (this repository) — the audit lane Decision 6 keeps out of this facade.
- mddlog issue #5 — the build/test baseline; this record adds no requirement to it.

## Approval
- **Decision Date**: not yet approved — drafted for review.
- **Approved By**: pending (project maintainer).
- **Review Date**: before any adapter code is written, and after re-verifying the WebFront baseline commit.
