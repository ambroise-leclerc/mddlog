# ADR-001: Allocation-free governed logging core for IEC 62304 Class C

## Status
Proposed — drafted for maintainer review, not yet acted on.

All MduX references in this record are pinned to commit
[`d972d77`](https://github.com/ambroise-leclerc/MduX/tree/d972d77bc5cefdbe105ad7933ee61746fb5eb45b),
the same baseline mddlog issue #5 verifies against, so the comparisons stay checkable as MduX moves.

## Context

`README.md` states mddlog is "real-time capable" and lists "Memory pool allocation for
zero-allocation logging" under Performance & Monitoring, marked `(planned)`. Nothing in the current
module set delivers that today, and several current design choices make it unreachable without a
structural change rather than an incremental one:

- `LogRecord` (`include/mddlog/core/LogRecord.cppm:18-48`) carries **nine** `std::string` data
  members (`message`, `category`, `userId`, `sessionId`, `deviceId`, `operationId`,
  `auditEventType`, `riskLevel`, `complianceStandard`) plus a
  `Metadata = std::unordered_map<std::string, std::string>`. Any of those values may allocate,
  depending on length and the implementation's small-string capacity; `addMetadata()` (line 121) may
  allocate for the map node and for key/value storage. The point is not that every field allocates
  every time — it is that the type provides no bound on whether it does.
- `getFormattedTimestamp()` and `getSourceLocationString()` (lines 138-158) build a
  `std::stringstream` per call and format through `std::put_time`/`std::gmtime`. `std::gmtime`
  returns a pointer into an implementation-shared buffer with no standard thread-safety guarantee;
  calling it from concurrent producers is a data race, which directly contradicts the "thread-safe"
  claim this same type is meant to support.
- `SimpleLogger` (`include/mddlog/core/Logger.cppm`) stores sinks as
  `std::vector<std::shared_ptr<Sink>>` (`SinkPtr`, `sinks/Sink.cppm:136`), queues records in an
  **unbounded** `std::queue<LogRecord>` (`logQueue_`, `Logger.cppm:373`), and swallows sink
  exceptions with bare `catch (...)` (`writeToSinks()`, lines 273-279; `flushSinks()`, lines
  290-294) — comments in both admit "In a medical device, this might need more sophisticated error
  handling" without providing it.
- `ConsoleSink::write()` (`sinks/ConsoleSink.cppm:44-113`) formats through `std::ostream` and wraps
  the whole body in `catch (const std::exception&)`.
- A second, unregistered copy of most of this (`LogRecord`, `Sink`, `ConsoleSink`, `SimpleLogger`)
  exists in the root-level `mddlog.cppm`, which `CMakeLists.txt` does not build
  (`target_sources(mddlog ... FILE_SET cxx_modules ...)` lists only `include/mddlog/mddlog.cppm` and
  its submodules). GitHub issue #5 already flags this as "the unused root-level `mddlog.cppm`
  ambiguity" to resolve; this ADR does not resolve it, but the allocation-free core defined below
  should not be duplicated into it once it is either deleted or reconciled.
- Issue #5 separately documents the unbounded queue as an accepted limitation ("Document the
  existing unbounded queue as a limitation; bounded real-time queue design is outside this issue")
  and treats "persistent/cryptographically protected audit storage" as out of its own scope — both
  are exactly the gap this ADR and ADR-002 exist to close.

None of this is a criticism of what exists — a synchronous/asynchronous best-effort logger with a
console sink is a reasonable first module set. The point of this ADR is that "zero-allocation
logging" and "Class C deployability" cannot be reached by tuning that design; they require a
boundary between code that is allowed to allocate, throw, or block, and code that is not.

## Medical Device Considerations

### IEC 62304 implications
- **Segregation for risk control.** IEC 62304 requires software items whose segregation is relied
  on for risk control to be identified, with the segregation's adequacy shown — the requirement
  MduX's ADR-004 builds its governed/adapter split on. This record deliberately does not quote the
  standard or pin a sub-clause number: mddlog holds no copy of IEC 62304, the base text and
  Amd 1:2015 number this material differently, and asserting a number this project cannot verify
  would be exactly the kind of unchecked claim the rest of this ADR argues against. A future
  revision may cite the precise clause once someone with the standard in hand confirms it.
- **Fail-closed behavior**: a governed logging call must have a bounded, defined outcome under
  resource exhaustion (refuse-and-count, never allocate-and-hope) rather than an unbounded queue
  that defers the failure to an unpredictable later moment, as `logQueue_` does today.
- **Bounded time is a separate property from bounded memory.** An allocation-free call is not
  automatically a time-bounded one. The governed path must therefore also exclude unbounded retry
  loops and blocking waits, and Decision 6's verification criteria must not be read as covering
  timing merely because they cover allocation.

### Risk management considerations
- A silently swallowed sink exception (`catch (...)` with no counter, no re-raise, no side channel)
  is a hazard in its own right for the audit-relevant levels: a caller believes an `AUDIT` or
  `ERROR` record was recorded when it may not have reached any sink at all. ADR-002 addresses the
  audit path specifically; the governed core defined here must not add a *second* unreported failure
  mode of its own — every refusal and every post-admission loss must be observable through the
  contract in Decision 3, not swallowed.

## Decision

Split mddlog into two zones, following the shape of
[MduX ADR-004](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/ADR-004-trust-zones-in-cpp.md),
adapted to a standalone library with no `mdux.governance`/`mdux.evidence` to import:

| Zone | Module(s) | May allocate / throw / block | Public API may contain |
|---|---|---|---|
| Governed core | `mddlog.core.record`, `mddlog.core.ring` (new) | No | `std::span`, fixed-capacity value types, `std::string_view` into caller-owned storage |
| Sinks / adapter | `mddlog.sinks.*`, `mddlog.adapter.logger` | Yes | `std::string`, `std::shared_ptr`, `std::ostream`, any allocating container |

### 1. `LogRecord` becomes a fixed-capacity, non-allocating value type

Replace every `std::string` field with a caller-sized inline buffer plus length, e.g.
`InlineString<N>` (a `std::array<char, N>` and a `std::uint16_t` length, `constexpr`, `noexcept`).
Capacities are declared in **bytes**, not code points; a value is accepted only if it fits whole, so
no field ever stores a partial UTF-8 sequence. `N` is constrained at compile time to the range the
`std::uint16_t` length can represent.

Drop `getFormattedTimestamp()`/`getSourceLocationString()`'s `std::stringstream`/`std::gmtime` path
from the governed type: a governed record stores a raw time value supplied by the host (ADR-002
Decision 5) and `std::source_location` (which is non-owning and fine as-is), and text formatting is
a sink-zone concern that may allocate.

**Context is captured at emission, not reconstructed at drain.** Free-form
`std::unordered_map` metadata leaves the governed zone, but it is *not* replaced by "the adapter
adds context later": by the time a consumer drains the ring, the active connection, call or
operation may be gone. The governed record therefore keeps a small, bounded context envelope filled
in by the producer — a component identifier, an operation/call identifier, and a correlation
identifier (ADR-002 Decision 4) — as fixed-capacity fields. This is what lets one generic record
serve both MduX's requirement/hazard references and WebFront's `WebLinkId`/`CallId` correlation
without imposing a medical schema on ordinary diagnostic logging. Unbounded key/value metadata, if
still wanted, is an adapter-zone decoration of an already-complete record, never the carrier of its
identity.

### 2. Truncation policy is per field kind, not global

Truncating a diagnostic message is acceptable and observable. Truncating an **identifier** is not:
two distinct control, requirement, actor or operation identifiers that share a prefix become
indistinguishable, which silently corrupts exactly the linkage ADR-002 depends on. Therefore:

- **Identifier-kind fields**: a value that does not fit is **refused**. The record is not published
  at all, and the caller receives a refusal naming the offending field. No partial identifier is
  ever stored.
- **Descriptive-text fields** (message, free-form description): may be truncated, and truncation is
  flagged on the record and reported in the call's result.

### 3. The result type expresses admission and truncation independently

A `bool` cannot carry this contract. The governed write returns a small value with two independent
components:

- **admission**: `Written` | `Refused(reason)` — where `reason` distinguishes at minimum
  `RingFull`, `IdentifierTooLong(field)` and `MalformedTime`;
- **truncation**: whether any descriptive field was shortened, and which.

The governed path returns this by value; it never throws, and contains no `try`/`catch`. This is a
narrower version of
[MduX ADR-005](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/ADR-005-error-handling-and-exceptions-policy.md);
if the governed surface grows past this record/ring pair, a follow-up ADR should adopt
`std::expected` wholesale rather than accumulate ad hoc result types one at a time.

### 4. `RingLog` is a single-producer/single-consumer queue with a stated publication protocol

A fixed-capacity array plus one write cursor is not yet a usable queue between an asynchronous
producer and an adapter — if the producer overwrites a slot while a sink is reading it, the reader's
view changes underneath it and the access is a data race. MduX's `SampleRing`
([`Trace.cppm`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/include/mdux/medui/Trace.cppm))
is *not* a precedent for this: it is a read-only view over samples the caller owns and re-validates
every frame, not a concurrent queue. So this ADR states the model explicitly rather than inheriting
one:

- **Concurrency model**: one producer, one consumer (SPSC) per `RingLog` instance. Multiple
  producers use one ring each, and the adapter aggregates across rings; this keeps the governed side
  lock-free without making cross-ring ordering a promise (ADR-002 Decision 5 covers ordering).
- **Cursors**: a write cursor and a **read cursor**, both monotonically increasing sequence counters
  rather than wrapped indices, so "empty", "full" and "how many were lost" are all derivable by
  subtraction and wrap-around is not ambiguous.
- **Publication**: a slot becomes visible to the consumer only when the write cursor is published
  (release), and becomes reusable by the producer only after the consumer advances the read cursor
  past it (acquire). A slot between those two points is never rewritten.
- **Overflow**: **refuse-new**, not drop-oldest. A ring with no reusable slot refuses the write with
  `Refused(RingFull)` and increments an observable refusal counter. Refusing a new record keeps
  every already-admitted record intact, which is the behavior ADR-002's audit lane requires; the
  earlier draft left drop-oldest open as an option, and this revision closes it, because an audit
  event that was accepted and then silently overwritten is strictly worse than one that was never
  accepted.
- **Drain view lifetime**: the consumer obtains the unread region as **one or two**
  `std::span<const LogRecord>` (two when the unread region wraps). Those spans are valid only until
  the consumer advances the read cursor; the consumer must finish copying out of them before
  acknowledging. Nothing outside that window may retain them.

### 5. Draining to a sink is an adapter-zone operation, with a stated field contract

Copying a drained record into a `Sink` (`sinks/Sink.cppm`), formatting it to text/JSON, doing file or
network I/O, and taking the mutex-guarded `std::vector<SinkPtr>` path all stay where they are today.
This ADR does not ask that code to become allocation-free, because console/file/network I/O cannot
be, by construction. What it does require is that the boundary be stated rather than assumed:

- The adapter copies out of the drain spans (Decision 4) before acknowledging; it never stores a
  span.
- A record's truncation flag (Decision 2) is carried through to the sink, so a sink can render a
  shortened message as shortened rather than as complete.
- **No existing sink preserves every field.** `ConsoleSink::write()`
  (`sinks/ConsoleSink.cppm:71-87`) emits `userId`, `deviceId`, `operationId`, `auditEventType` and
  `riskLevel`, but not `complianceStandard`, and its output is a human-readable line, not a
  round-trippable encoding. Nothing in this ADR should be read as implying that routing a governed
  record through today's sinks preserves it; the sink that is responsible for complete audit-field
  preservation is the one ADR-002's deferred persistence ADR will have to specify.

### 6. The boundary is materialised as modules *and* CMake targets

A table in a document is not a boundary. Today `SimpleLogger` lives in `mddlog.core.logger` and
imports `mddlog.sinks.sink`, so the `mddlog.core.*` prefix does not currently mean "governed"; and
`CMakeLists.txt` builds a single `mddlog` target containing every module. Both must change for the
split to mean anything:

- The governed modules move under names that only contain governed code (`mddlog.core.record`,
  `mddlog.core.ring`). `SimpleLogger` moves out of the `mddlog.core.*` namespace to an adapter
  module — a rename, not a rewrite; the existing facade keeps working for existing callers.
- Two CMake targets with a **one-way** dependency: `mddlog-core` (governed, no sink modules) and
  `mddlog` (adapter, links `mddlog-core`). A consumer — MduX being the motivating one — must be able
  to import and link the governed core without pulling in `SimpleLogger`, the sinks, or their
  dependencies.

Verification criteria for that boundary, in decreasing strength: a link/import dependency-graph
check; an object scan over the compiled governed target; and source-level checks for forbidden
constructs. mddlog has **no CI workflow at all yet** (issue #5), so none of these exist today and
this ADR must not be read as claiming them. Three limits apply even once they do:

- an allocation scan shows no allocation; it shows nothing about blocking or bounded time;
- MduX's two scan profiles are not interchangeable: `ml-noheap` is the strict no-allocation profile
  over specific objects, while `governed-throw` checks for throws with tooling-dependent limits and
  does **not** forbid allocation
  ([`MduXNoHeapScan.cmake`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/cmake/MduXNoHeapScan.cmake));
- a `-fno-exceptions` build must not be promised from the shape of these types. MduX ADR-005 records
  that `import std` and `-fno-exceptions` are mutually exclusive on GCC because the dialect is
  recorded in the module BMI — mddlog uses `import std` too, so the same constraint applies here
  until demonstrated otherwise on a specific toolchain.

## Alternatives Considered

### 1. Keep one `LogRecord`/`SimpleLogger`, gate allocation behind a compile-time flag (Rejected)
**Pros:** No new types, no second module family.
**Cons:** The allocating and non-allocating code would still share the same translation units and
the same `std::string` member layout; the property would depend on the flag always being set
correctly rather than on a type that cannot allocate regardless of how it is compiled. MduX's
ADR-004 rejected the equivalent option for the same reason.

### 2. A PMR/arena allocator instead of fixed-capacity fields (Rejected for the governed core)
**Pros:** Keeps `std::string`-like ergonomics. And, contrary to this ADR's first draft, it *can* be
heapless: `std::pmr::monotonic_buffer_resource` can be constructed over caller-provided storage with
no initial heap allocation, and giving it `std::pmr::null_memory_resource()` as upstream keeps it
heapless when that storage is exhausted.
**Cons:** What it does not give is a *bound*. Exhaustion surfaces as `std::bad_alloc` from the
upstream resource — an exception on the governed path, which Decision 3 exists to eliminate — and
nothing in the container API expresses "this value was refused because it did not fit" as an
ordinary result the caller must handle. Fixed-capacity fields make the limit part of the type and
the refusal part of the return value. PMR remains a reasonable optimization for the **sink zone**,
where allocation and exceptions are already permitted; that is a separate decision.

### 3. Drop-oldest on ring overflow (Rejected — see Decision 4)
The first draft of this ADR left this open. It is closed here in favour of refuse-new, because
drop-oldest makes an already-admitted record disappear without its producer learning, which ADR-002
cannot build an audit delivery contract on. A diagnostic-only ring may legitimately want
drop-oldest; if that is ever wanted, it should be a distinct, explicitly named ring type rather than
a mode of this one.

## Consequences

### Positive
- Makes "zero-allocation logging" and Class C deployability reachable goals with a concrete type
  shape, rather than open-ended README bullets.
- Reuses a boundary pattern MduX has already worked through — including the parts MduX found the
  hard way, such as the `import std`/`-fno-exceptions` interaction — which is design *precedent*,
  not transferred validation (see Risks).
- Moving formatting out of the governed record would remove the `std::gmtime` data race from the
  governed path when implemented. It does not fix it today: `LogRecord::getFormattedTimestamp()`
  still calls `std::gmtime`, and that defect remains open regardless of this ADR's status.

### Negative
- Introduces a second `LogRecord`-shaped type (or a capacity-parameterized template), more surface
  to document and keep in sync than the current single struct.
- Fixed capacities make some inputs refusable that are accepted today (Decision 2), which is a
  behavioral change for existing callers, not only an internal one.
- The module rename and target split (Decision 6) touch the public module names, so they need a
  migration note for any existing consumer.
- No mechanical enforcement exists yet; this ADR makes the boundary reviewable, not enforced.

### Risks and Mitigations
- **The precedent is overstated.** MduX describes itself as experimental; its ADRs, dependency
  checks and targeted tests are an architectural precedent and a set of specific verifications, not
  production validation transferable to mddlog. *Mitigation*: this revision removes the "already
  validated"/"battle-tested" wording the first draft used, and states what each borrowed mechanism
  actually checks (Decision 6).
- **"Allocation-free core" is read as "the library is Class C".** It is not: an allocation-free core
  makes integration into a device under strong constraints easier, and establishes no qualification
  of mddlog itself. *Mitigation*: keep this sentence in the ADR, and keep README's `(planned)`
  markers honest.
- **The boundary is stated but not enforced, and drifts under later edits.** *Mitigation*: the
  dependency-graph and object-scan checks in Decision 6 must be tracked as a named follow-up before
  this ADR moves to Accepted — MduX's own ADR-004 had to be amended in 2026-08 because it asserted a
  lint that did not yet exist.
- **Refuse-new starves a slow consumer's producer under sustained load.** *Mitigation*: refusals are
  counted and observable (Decision 3), so sustained refusal is a visible condition the host can act
  on, rather than a silent one — but the host, not the logger, decides what to do about it.

## References
All MduX links pinned to `d972d77bc5cefdbe105ad7933ee61746fb5eb45b`.
- [MduX ADR-004: Trust zones in C++](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/ADR-004-trust-zones-in-cpp.md) — the governed/adapter split this ADR adapts.
- [MduX ADR-005: Error handling and exceptions policy](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/ADR-005-error-handling-and-exceptions-policy.md) — the no-throw rationale, and the `import std`/`-fno-exceptions` limit cited in Decision 6.
- [MduX `Trace.cppm`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/include/mdux/medui/Trace.cppm) — `SampleRing`, cited in Decision 4 for what it is *not* a precedent for.
- [MduX `cmake/MduXNoHeapScan.cmake`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/cmake/MduXNoHeapScan.cmake) — the two scan profiles and their different scopes.
- mddlog issue #5 — the unbounded queue, the root-level `mddlog.cppm` ambiguity, and the absent CI this ADR's Decision 6 depends on.
- ADR-002 (this repository) — the audit delivery contract built on Decision 3 and Decision 4.

## Approval
- **Decision Date**: not yet approved — drafted for review.
- **Approved By**: pending (project maintainer).
- **Review Date**: when a follow-up issue implementing `RingLog`/`InlineString` is opened, or when issue #5's build/test work lands, whichever is first.
