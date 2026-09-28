# ADR-003: Application integration and sink ownership

## Status
Proposed — drafted for maintainer review, not yet acted on.

Requested in the review of PR #6: ADR-001 and ADR-002 define a bounded core and an audit contract,
neither of which is sufficient to replace an existing application logger. This record covers the
integration boundary, using [WebFront](https://github.com/ambroise-leclerc/WebFront) as the concrete
consumer because it is the one that exists.

**Baseline for this record**: WebFront
[`7be626ccfbb50524c9c03296e6c84555ea7d2c7c`](https://github.com/ambroise-leclerc/WebFront/tree/7be626ccfbb50524c9c03296e6c84555ea7d2c7c).
The PR review cited `d927b05a…`; this record was drafted from a later working copy at `7be626c`,
and every claim below names the file and line it was read from there. Issue #66 re-read every
reference from a fresh clone of that commit on 28 September 2026, when it was also the head of
WebFront's `develop` branch; it is the pinned baseline for implementation. The corrections that
revalidation required are applied in place and listed in [Revalidation at `7be626c`](#revalidation-at-7be626c-66).
Revalidation does not change the status above: acceptance remains a separate maintainer decision.

## Context

### What WebFront's logger is today

`webfront::log` (`include/tooling/Logger.hpp`, 71 lines) is header-only and pre-modules:

- **Levels are `const uint8_t` constants**, not an enum class: `webfront::log::Disabled = 0, Error = 1,
  Warn = 2, Info = 3, Debug = 4` (line 18). The numeric order is **inverted** relative to mddlog's
  `LogLevel` (`LogLevel::Trace = 0 … LogLevel::Fatal = 5`, where higher means more severe; #58
  removed the former `LogLevel::Audit = 6`, and audit is no longer a severity). Both sides
  now spell `Error`, `Warn`, `Info` and `Debug` identically, with different numeric values, so the
  qualifier (`webfront::log::` or `LogLevel::`) is what tells them apart below. Any mapping must be written out rather
  than assumed to be a cast.
- **Enablement is per level, not a threshold**: `inline bool logTypeEnabled[Debug + 1]` (line 20)
  with `set()`, `is()` and `setLogLevel()` (lines 55-57). `test/LoggerTests.cpp:37-69` pins this
  precisely — it disables `Warn` and `Error` while leaving `Info` and `Debug` on, then inverts the
  combination, and asserts the resulting level characters in order (`{'I','D','W','E','W','I','E'}`).
  mddlog's single `minLevel` threshold **cannot express that**; this is a capability difference, not
  a naming difference. The array is zero-initialized, so **every level starts disabled** until
  `set()` or `setLogLevel()` runs, and it is a plain `bool` array written and read without
  synchronization.
- **Formatting is eager, at the call site**: `std::format`/`vformat` produce a `std::string` which is
  passed to every sink synchronously (lines 36-53). There are **two rendered shapes**: `debug` renders
  `[D] HH:MM:SS | file:line | text` with the filename padded to 16 and the line to 4 columns
  (line 38), while `error`, `warn`, `info` and `infoHex` render `[X] HH:MM:SS | text` with no
  location (line 36). The timestamp is `{:%T}` of `system_clock::now()` — **time of day only, no
  date** — and, because that clock's duration is finer than a second, `%T` also prints a fractional
  second whose precision is the standard library's (nanoseconds in libstdc++, microseconds in libc++).
  `LoggerTests.cpp` asserts only the prefix and suffix of a line, not the timestamp.
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

   At this baseline the removal is also reached later than a disconnect. `BasicWF` erases a link
   only on `WebLinkEvent::Code::closed` (`WebFront.hpp:360-361`), and nothing in WebFront emits that
   code: the WebSocket close handler rejects pending calls (`WebLink.hpp:62-65`) but does not
   destroy the link. A disconnected `WebLink` therefore keeps its browser sink registered, and every
   later log line is still handed to it, until `BasicWF` itself is destroyed. The adoption work must
   define when a link is destroyed before quiescent removal (Decision 4) can bound that lifetime.
3. **`addSinks` returns only the last id.** It is a fold over `push_back` followed by
   `return out.sinks.size() - 1` (line 69), so registering several sinks in one call makes every id
   but the last unrecoverable, and therefore unremovable.

### Why the browser sink cannot use a naive path

The sink registered at handshake sends the log line over the same WebSocket it is logging about:
`logSink = log::addSinks([this](std::string_view t) { sendCommand(msg::TextCommand(msg::TxtOpcode::debugLog, t)); });`
(`WebLink.hpp:120`). `sendCommand` calls `ws->write` (line 80). `WebLink` logs on many of the paths
that reach a write — construction (line 58), every binary message via `log::infoHex` (line 103),
`log::error` on an empty message (line 105), `log::info` in `handleCallFunction` (lines 132, 142),
and `log::error` for a return to an unknown call, emitted while `pendingMutex` is held (line 226).
Two paths re-enter the browser sink:

- **Synchronously, while building a frame.** `Frame::addBuffer` logs at debug level
  (`WebSocket.hpp:165-166`), and every parameter encoder calls it (`Messages.hpp:312-389`). So
  `sendException` and `sendError` (`WebLink.hpp:148-154`, `199-208`) re-enter the sink with one
  `sendCommand` per encoded buffer while their own frame is still being built. `sendCommand` itself
  does not recurse: the frame constructors it uses do not log (`WebSocket.hpp:119-132`).
- **Asynchronously, after a failed write.** The write-completion handler logs
  `log::error("Error during write …")` (`WebSocket.hpp:366-368`), which hands a new line to the
  browser sink of the same failing socket, which queues another write. Only the `started` flag,
  cleared by `stop()` (`WebSocket.hpp:276-277`, `367-370`), ends that loop.

Today nothing else breaks either cycle; a transport that starts failing and logging its failures is
the amplification case the review names.

### What correlation already exists

WebFront already has the identifiers ADR-001's emission-time context envelope needs:
`WebLinkId = uint16_t` (`WebLink.hpp:23`), `msg::CallId` allocated per link from `nextCallId{1}`
(line 52), `WebLinkEvent` carrying both (lines 25-35), and `expectResult()` returning
`{CallId, std::future<Result>}` with a `pendingCalls` map under `pendingMutex` (lines 84-99). The
integration does not need to invent correlation; it needs to stop discarding it into a pre-formatted
string.

Both identifiers are 16-bit and **reused**. `CallId` (`Messages.hpp:24`) wraps, skipping 0 and ids
still pending (`WebLink.hpp:211-218`). `WebLinkId` is allocated from `idsCounter`, which wraps and
skips ids still present in `webLinks` (`WebFront.hpp:237-238`, `348`). A `(webLinkId, callId)` pair
therefore identifies one call only while it is outstanding, not across a process lifetime. A
consumer that must join records beyond that window needs a further discriminator, such as the link's
creation time or a host-issued session identity.

### The consumption gap

WebFront is header-only, `cmake_minimum_required(VERSION 3.31)`, `cxx_std_23`
(`CMakeLists.txt:1, 33`). mddlog requires CMake 4.0 through 4.3, C++23 **modules** and `import std`
(`CMakeLists.txt:1, 7-10`), and admits only MSVC 17.14+, GCC 16.1+ (excluding 16.2) or upstream
Clang 20+, refusing AppleClang (`CMakeLists.txt:30-60`). Both using C++23 does not make one
consumable by the other, and this record must not imply otherwise.

## Medical Device Considerations

This record is mostly not a medical-device decision: WebFront is a general-purpose UI library, and
forcing an audit model onto it would be the wrong direction. Two points do matter:

- **The audit lane stays optional and separate.** ADR-002's `AuditEvent` path must not be enabled by
  default for a consumer like this, and audit events must never be routed to a browser sink as
  ordinary diagnostics — a regulatory record streamed to a web client as debug text is neither
  protected nor bounded. Since #57 and #58 the separation is structural: audit events are admitted
  only through `AuditRing` and delivered only to an `AuditSink`, a type that cannot be installed
  where a diagnostic `Sink` is expected, and no `LogLevel` names audit.
- **Diagnostics are not evidence.** Anything this integration streams is a diagnostic aid. Nothing in
  it supports a traceability or audit claim, and the facade should make that hard to confuse.

## Decision

### 1. A facade, with the existing test suite as the contract

Provide a `webfront::log`-shaped facade over mddlog rather than asking call sites to change:
`debug`/`info`/`warn`/`error`/`infoHex`, `set`/`is`/`setLogLevel`, `addSinks`/`removeSinks`. The
behaviors pinned by `test/LoggerTests.cpp` are the acceptance criteria — the level character in
position 1, the message at the end of the line, the filename in a debug line, and the exact
enable/disable sequence of lines 37-69. A migration that changes the rendered shape breaks that
suite, and the suite is right to break. The facade reproduces both shapes described in Context — with
location for `debug`, without it for the other levels — and starts with every level disabled, as
the zero-initialized array does today.

### 2. Per-level enablement, not a threshold

mddlog's `minLevel` cannot express "Warn off, Error on". The adapter therefore carries a per-level
enable mask and maps it explicitly:

| WebFront | mddlog |
|---|---|
| `webfront::log::Error` (1) | `LogLevel::Error` (4) |
| `webfront::log::Warn` (2) | `LogLevel::Warn` (3) |
| `webfront::log::Info` (3) | `LogLevel::Info` (2) |
| `webfront::log::Debug` (4) | `LogLevel::Debug` (1) |
| `webfront::log::Disabled` (0) | all levels masked off |

`LogLevel::Trace` and `LogLevel::Fatal` have no WebFront equivalent: `LogLevel::Trace` maps into
`webfront::log::Debug` for display and `LogLevel::Fatal` into `webfront::log::Error`. Audit is not a
`LogLevel` since #58, and `AuditEvent` is **not routed to this facade at all** (Decision 6).

**The mask lives in the adapter** (settled by #66). mddlog's `SimpleLogger` keeps its single
threshold: no other consumer needs per-level enablement, and adding it there would change the
filtering semantics every existing mddlog user relies on for one consumer's benefit. The adapter's
mask is a set of atomic per-level flags, so `set()` concurrent with logging is not the data race the
plain `bool` array is today.

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

**Option 2 is adopted, with this split of responsibilities** (settled by #66):

- **mddlog** gains the module-side pieces that are general, not WebFront-specific: the sink registry
  of Decision 4 and the bounded transport consumer of Decision 5, in the adapter zone and registered
  in the `mddlog` target like the existing adapters. mddlog ships no header-only surface, consistent
  with its README ("Pure C++23 modules library") and Alternative 2 below.
- **WebFront** owns the `webfront::log` facade header, which imports no modules, and the single
  translation unit that imports mddlog to implement it. Both are built only behind a WebFront CMake
  option that defaults to off. With the option off, WebFront keeps its current logger, CMake 3.31
  floor and header-only build unchanged.

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
single-sink registration the shape is unchanged.

**Spelling** (settled by #66): `addSinks(sink)` returns one handle, and `addSinks(s1, …, sN)` with
N > 1 returns `std::array<Handle, N>` in argument order. An array rather than a tuple, because every
element has the same type and a caller can iterate it to remove them all. The WebFront revalidation
found **no multi-sink call site**: all four calls at the baseline register a single sink
(`WebLink.hpp:120`, `test/LoggerTests.cpp:17`, `src/HelloWorld.cpp:31`,
`webtest/JasmineTest.cpp:209`). The break is therefore a compile-time change to the API contract,
with nothing to migrate in WebFront itself.

### 5. The transport consumer is bounded, non-reentrant, and fails quietly

For a sink that writes to a transport (the browser sink being the motivating case):

- **Bounded**: it consumes from a bounded ring (ADR-001 Decision 4), not from the emitting thread. A
  slow or stalled browser must not block a producer, and saturation refuses with an observable
  counter rather than growing without bound.
- **Non-reentrant**: logs emitted while dispatching to a sink must not re-enter that sink. A
  thread-local "in dispatch" guard (or a dedicated consumer thread that never logs through the
  registry) breaks both cycles documented in Context: the synchronous one through
  `Frame::addBuffer` while an error frame is encoded, and the asynchronous one through the
  write-error log.
- **Fails quietly and locally**: a transport error detaches the sink and is reported through the
  registry's own health counters, not by logging the failure through the path that just failed.
- **Disconnection is ordinary**: sink removal at `~WebLink` follows Decision 4, so a disconnect in
  flight is a wait, not a race. That holds only once a disconnect actually leads to `~WebLink`. At
  the baseline it does not (Context, defect 2), so the WebFront adoption must emit, or otherwise act
  on, the close so the link and its sink are retired when the connection ends.
  `~WebLink` also logs before removing its own sink (`WebLink.hpp:75-77`). The adoption must remove
  the sink first, or that last line is sent to the connection being torn down.

### 6. Context is captured at the producer; formatting happens in the adapter; audit stays out

Following ADR-001 Decision 1, the facade passes `WebLinkId`, `CallId` and a component identifier
through as *fields*, captured when the call is made — not folded into a pre-formatted string as
today. The adapter renders the final text, including both existing shapes (`[D] HH:MM:SS | file:line | text`
and `[X] HH:MM:SS | text`), so Decision 1's test contract still holds while the identifiers survive
to any other sink.

`AuditEvent` (ADR-002) is not reachable through this facade. A consumer that wants an audit lane opts
into it explicitly, and it does not share the browser sink.

**Worked example — one call's request, response and error, across two simultaneous connections.**
The identifier that matters is the **pair**, not the `CallId`: `WebLinkId` is allocated per
connection (`WebFront.hpp:237-238`, from the counter declared at line 348) while `CallId` restarts
from `nextCallId{1}` inside each `WebLink` (`WebLink.hpp:52`), so call 1 on link 1 and call 1 on link 2
are unrelated calls that today render as indistinguishable text. Because both counters wrap
(Context), the pair identifies a call while it is outstanding, which is the window this example
covers.

Two browsers are connected as links 1 and 2. Each invokes a JS function; `expectResult()` allocates
a `CallId` per link and returns a future (`WebLink.hpp:84-99`), `JsFunction` stamps it on the outgoing
command (`include/JsFunction.hpp:41`), the reply arrives as `functionReturn` and settles through
`completePending` (`WebLink.hpp:112`, `220-233`), and a failure arrives instead as an encoded
exception or an error (`sendException` and `sendError`, called at lines 139 and 143 and defined at
148-154 and 199-208) — or, if the browser disconnects mid-call, as `rejectPending` from the close
handler (line 64):

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
consumer drains the ring, link 1 may already be destroyed (`~WebLink`, line 74) — at the latest
once the adoption retires links at disconnect (Decision 5) — so there is nothing left to ask.

## Alternatives Considered

### 1. WebFront depends on mddlog directly (Rejected)
**Pros:** No adapter layer; one logger.
**Cons:** Imposes CMake 4, C++23 modules and `import std` on a header-only CMake 3.31 consumer, and
makes WebFront's toolchain floor a function of mddlog's. Decision 3 option 1/2 gets the same
functionality without that.

### 2. mddlog ships a header-only shim (Rejected as a default)
**Pros:** Simplest possible consumption story.
**Cons:** mddlog's README describes a "Pure C++23 modules library" with "No legacy headers"; a shipped
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
- Documents the re-entrancy cycles as existing code paths rather than hypotheticals.
- Keeps mddlog's modules-only public surface intact while giving a pre-modules consumer a supported
  path.

### Negative
- A facade plus an adapter TU is a second public surface to maintain, and it exists only for
  consumers that cannot take modules.
- Per-level masking (Decision 2) is capability the mddlog core does not have; it lands in the
  adapter as new code with its own tests, and mddlog's `SimpleLogger` keeps its threshold.
- Removal-waits-for-quiescence (Decision 4) is more expensive than nulling a slot, and needs care not
  to deadlock when a sink is removed from inside a sink callback.

### Risks and Mitigations
- **The baseline moved.** This record was read at WebFront `7be626c`, not the `d927b05` the review
  cited. *Mitigation*: every claim names file and line, and #66 re-verified them at `7be626c`, which
  is now the pinned baseline. If WebFront changes a cited file before adoption, the adoption PR
  re-reads the affected claims rather than relying on this record.
- **The facade freezes a rendered text shape.** `LoggerTests.cpp` asserts on substrings of the
  formatted line, so the adapter inherits a format contract it did not choose. *Mitigation*: treat
  the shape as versioned by that test, and change it only by changing the test deliberately.
- **A fix for re-entrancy hides a real transport failure.** Suppressing logs during dispatch means a
  failing transport reports less, not more. *Mitigation*: health counters in the registry (Decision
  5) are the reporting channel, and they are not routed through sinks.

## Revalidation at `7be626c` (#66)

Every file and line reference above was re-read on 28 September 2026 from a fresh clone of WebFront
`7be626ccfbb50524c9c03296e6c84555ea7d2c7c`, then the head of its `develop` branch, together with
mddlog's `develop` after #58. The references not listed below were confirmed as written. The
following were corrected in place:

| Claim | Correction |
|---|---|
| Rendered shape `[X] HH:MM:SS \| file:line \| text` | Only `debug` carries a location; the other levels render `[X] HH:MM:SS \| text`. `%T` includes a fractional second. |
| `WebLinkEvent` carries both ids, lines 28-34 | Lines 25-35. |
| `sendException`/`sendError`, lines 139-153 | Called at 139 and 143; defined at 148-154 and 199-208. |
| `WebLinkId` allocated at `WebFront.hpp:347-348` | Allocated at 237-238; the counter is declared at 348. |
| `JsFunction.hpp:41` | Path is `include/JsFunction.hpp`; line confirmed. |
| mddlog floor at `CMakeLists.txt:1-7, 29-43` | CMake 4.0 to 4.3 at lines 1 and 7-10; compiler admission at 30-60. |
| `LogLevel::Trace = 0 … LogLevel::Audit = 6` | #58 removed `Audit`; `LogLevel` ends at `Fatal = 5`. |

Facts the draft did not record, now in Context and Decisions:

- Every level starts disabled, and the enable array is unsynchronized.
- Both correlation identifiers are 16-bit and reused after wrap.
- A second, synchronous re-entrancy path through `Frame::addBuffer`, and the `started` flag that
  alone bounds the asynchronous one.
- `log::error` under `pendingMutex` (`WebLink.hpp:226`).
- Nothing emits `WebLinkEvent::Code::closed`, so links and their browser sinks outlive their
  connections.
- `~WebLink` logs before removing its own sink.

Points the draft left to implementation, settled here: the mask stays in the adapter
(Decision 2); option 2 with mddlog owning the registry and transport consumer and WebFront owning
the facade and adapter TU behind an off-by-default option (Decision 3); `std::array` of handles for
multi-sink registration (Decision 4).

Usage census at the baseline, for the implementing issues:

| File | Logger calls |
|---|---|
| `include/frontend/CEF.hpp` | `info` ×8 |
| `include/http/HTTPServer.hpp` | `debug` ×6, `info` ×3, `warn` ×1, `error` ×1 |
| `include/http/WebSocket.hpp` | `debug` ×7, `error` ×2 |
| `include/networking/NetworkingMock.hpp` | `debug` ×7 |
| `include/tooling/PathUtils.hpp` | `info` ×4 |
| `include/weblink/WebLink.hpp` | `debug` ×3, `info` ×2, `error` ×2, `infoHex` ×1, `addSinks` ×1, `removeSinks` ×1 |
| `src/HelloWorld.cpp` | `setLogLevel` ×1, `addSinks(clogSink)` ×1, `info` ×1 |
| `webtest/JasmineTest.cpp` | `setLogLevel` ×1, `addSinks(clogSink)` ×1, `error` ×6, `info` ×2 |
| `test/WebFrontTests.cpp` | `debug` ×1 |
| `test/LoggerTests.cpp` | the facade contract of Decision 1 |

No call site outside `LoggerTests.cpp` uses `set()` or `is()`. None registers more than one sink,
and only `WebLink` removes one; the two programs keep `clogSink` for the process lifetime.

## References
- WebFront at `7be626ccfbb50524c9c03296e6c84555ea7d2c7c`: `include/tooling/Logger.hpp`,
  `include/weblink/WebLink.hpp`, `include/weblink/Messages.hpp`, `include/http/WebSocket.hpp`,
  `include/WebFront.hpp`, `include/JsFunction.hpp`, `include/tooling/HexDump.hpp`,
  `test/LoggerTests.cpp`, `src/HelloWorld.cpp`, `webtest/JasmineTest.cpp`, `CMakeLists.txt`.
- ADR-001 (this repository) — the bounded ring Decision 5 consumes, and the emission-time context
  envelope Decision 6 relies on.
- ADR-002 (this repository) — the audit lane Decision 6 keeps out of this facade.
- mddlog issue #5 — the build/test baseline; this record adds no requirement to it.

## Approval
- **Decision Date**: not yet approved — drafted for review.
- **Approved By**: pending (project maintainer).
- **Review Date**: before any adapter code is written. The WebFront baseline was re-verified by #66;
  that revalidation is input to the review, not a substitute for it.
