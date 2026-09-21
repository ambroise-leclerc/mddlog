# ADR-001: Allocation-free governed logging core for IEC 62304 Class C

## Status
Proposed — drafted for maintainer review, not yet acted on.

## Context

`README.md` states mddlog is "real-time capable" and lists "Memory pool allocation for
zero-allocation logging" under Performance & Monitoring, marked `(planned)`. Nothing in the current
module set delivers that today, and several current design choices make it unreachable without a
structural change rather than an incremental one:

- `LogRecord` (`include/mddlog/core/LogRecord.cppm:18-48`) carries eight `std::string` fields plus a
  `Metadata = std::unordered_map<std::string, std::string>`. Constructing one allocates on every
  non-empty field; `addMetadata()` (line 121) allocates again per call.
- `getFormattedTimestamp()` and `getSourceLocationString()` (lines 138-158) build a
  `std::stringstream` per call and format through `std::put_time`/`std::gmtime`. `std::gmtime`
  returns a pointer into a implementation-shared buffer with no standard thread-safety guarantee;
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
logging" and "Class C" cannot be reached by tuning that design; they require a boundary between
code that is allowed to allocate, throw, or block, and code that is not, the same way MduX found for
its own governed/adapter split.

## Medical Device Considerations

### IEC 62304 implications
- **Segregation for risk control** (IEC 62304:2006 §5.3.3): a logging call on a Class C control path
  must not itself become a hazard source. An allocation that can throw `std::bad_alloc`, a mutex
  that can block a higher-priority thread, or an unbounded queue that can exhaust memory are each
  failure modes the *logger* would introduce into a device that otherwise controls its own memory
  and timing budget.
- **Fail-closed behavior**: a governed logging call must have a bounded, defined outcome under
  resource exhaustion (drop-and-count, never allocate-and-hope) rather than an unbounded queue that
  defers the failure to an unpredictable later moment, as `logQueue_` does today.

### Risk management considerations
- A silently swallowed sink exception (`catch (...)` with no counter, no re-raise, no side channel)
  is a hazard in its own right for the audit-relevant levels: a caller believes an `AUDIT` or
  `ERROR` record was recorded when it may not have reached any sink at all. This ADR's core is
  narrower than fixing that (ADR-002 addresses the audit path specifically), but the governed core
  defined here must not add a *second* unreported failure mode of its own — it must report every
  drop through the delivery-outcome contract, not swallow it.

## Decision

Split mddlog into two zones, named after the pattern
[MduX's ADR-004](https://github.com/ambroise-leclerc/MduX/blob/main/docs/adr/ADR-004-trust-zones-in-cpp.md)
already validated for the same "Class A/B vs Class C deployability" problem, adapted to a
standalone library with no `mdux.governance`/`mdux.evidence` to import:

| Zone | Module(s) | May allocate / throw / block | Public API may contain |
|---|---|---|---|
| Governed core | `mddlog.core.*` (new shape below) | No | `std::span`, fixed-capacity value types, `std::string_view` into caller-owned storage |
| Sinks / adapter | `mddlog.sinks.*` | Yes | `std::string`, `std::shared_ptr`, `std::ostream`, any allocating container |

### 1. `LogRecord` becomes a fixed-capacity, non-allocating value type

Replace every `std::string` field with a caller-sized inline buffer plus length, e.g.
`InlineString<N>` (a `std::array<char, N>` and a `std::uint16_t` length, `constexpr`,
`noexcept`, truncating rather than allocating on overflow). Replace `Metadata`
(`std::unordered_map<std::string, std::string>`) with a fixed-capacity
`std::array<KeyValue, MaxMetadataEntries>` the caller pre-sizes at compile time, or drop free-form
metadata from the governed record entirely and let the adapter zone attach it after the fact. Drop
`getFormattedTimestamp()`/`getSourceLocationString()`'s `std::stringstream`/`std::gmtime` path from
the governed type; a governed record stores the raw `std::chrono::system_clock::time_point` and
`std::source_location`, and formatting to text is a sink-zone concern that may allocate.

`std::source_location` itself is not owning — it is fine in the governed record as-is.

### 2. The record sink is a caller-supplied ring buffer, not an owned queue

Following the same pattern as MduX's `SampleRing` in
[`mdux.medui.trace`](https://github.com/ambroise-leclerc/MduX/blob/main/include/mdux/medui/Trace.cppm):
a governed `RingLog<Capacity>` is a fixed-size, caller-owned array of `LogRecord` plus a write
cursor. `log()` on the governed path never allocates: at capacity it overwrites the oldest entry (or
increments a dropped-record counter and refuses, whichever policy is chosen — see Alternatives) and
returns whether the write succeeded. This directly replaces the unbounded `std::queue<LogRecord>`
that issue #5 already flags as a documented limitation, for the governed path specifically; the
existing async/unbounded `SimpleLogger` may remain as-is in the sink/adapter zone for non-Class-C
use, since nothing in this ADR requires removing it.

### 3. No throw, no `catch`, in the governed core

The governed `log()`/drain path returns a bounded result (a `bool`, or an enum such as
`LogResult::Written | LogResult::Dropped | LogResult::Truncated`) rather than throwing or relying on
a caller's `try`/`catch`. This is a smaller version of
[MduX's ADR-005](https://github.com/ambroise-leclerc/MduX/blob/main/docs/adr/ADR-005-error-handling-and-exceptions-policy.md):
this project does not yet need `std::expected` machinery to state the same rule for its one
allocation-free type, but a future ADR should adopt `std::expected` wholesale if the governed
surface grows past this single record/ring pair, rather than accumulate ad hoc result enums one at
a time.

### 4. Draining to a sink is an adapter-zone operation

A governed `RingLog` only accepts writes and exposes a bounded, `noexcept` drain view (e.g.
`std::span<const LogRecord>` over the unread portion). Copying that view to a `Sink`
(`sinks/Sink.cppm`), formatting it to text/JSON, doing file or network I/O, and taking the
mutex-guarded `std::vector<SinkPtr>` path all stay exactly where they are today, in
`mddlog::sinks`/`SimpleLogger` — this ADR does not ask any of that code to become allocation-free,
because console/file/network I/O cannot be, by construction.

### 5. Verification is deferred, and named as a follow-up rather than assumed

MduX enforces its no-heap claim mechanically, post-build, with a symbol scan over the compiled
object (`cmake/MduXNoHeapScan.cmake`'s `ml-noheap` profile: no reference to `operator new/delete` or
`malloc` in the governed objects). mddlog has **no CI workflow at all yet** (issue #5) and no
equivalent scan. This ADR's Decision describes the source-level shape; a follow-up issue must add an
equivalent object-scan check before "allocation-free" is stated as a verified property rather than
a design intent, to avoid this project repeating the pattern MduX's own `docs/regulatory-compliance.md`
had to correct: a claim in prose outrunning what is mechanically checked.

## Alternatives Considered

### 1. Keep one `LogRecord`/`SimpleLogger`, gate allocation behind a compile-time flag (Rejected)
**Pros:** No new types, no second module family.
**Cons:** The allocating and non-allocating code would still physically share the same translation
units and the same `std::string` member layout; the guarantee would depend on the flag always being
set correctly rather than on a type that cannot allocate regardless of how it is compiled. MduX
rejected the equivalent option in its own ADR-004 for the same reason.

### 2. A PMR/arena allocator instead of fixed-capacity fields (Rejected for the governed core)
**Pros:** Keeps `std::string`-like ergonomics; one upfront allocation instead of none.
**Cons:** Still allocates once, so it cannot support "-fno-exceptions"-class deployments that forbid
a heap at all, and a `std::pmr` container still throws `std::bad_alloc` on the growth path
`operator new` would take if the arena is exhausted, which reintroduces the same halting problem
this ADR exists to remove. May be worth revisiting for the sink zone, where allocation is already
permitted, as a performance optimization — that is a separate, non-governed-zone decision.

### 3. Drop-oldest vs. refuse-new, on ring overflow (Undecided — flagged, not resolved here)
**Drop-oldest** favors "most recent state observable" (useful for a live telemetry view).
**Refuse-new** favors "nothing already accepted is ever silently discarded to make room for a new,
possibly less important, record" (arguably closer to an audit expectation). This ADR does not pick
one, because the answer likely differs between an ordinary trace record and an audit-category record
(ADR-002) — recommend resolving it per-category in the ADR-002 discussion rather than as a single
global ring policy.

## Consequences

### Positive
- Makes "zero-allocation logging" and "Class C" reachable claims instead of aspirational README
  bullet points, with a concrete type shape rather than an open-ended TODO.
- Reuses a boundary pattern already reviewed and battle-tested in MduX's own ADR-004/ADR-005,
  reducing design risk for a project at mddlog's current maturity (no test suite, no CI, per
  issue #5).
- Fixes the `std::gmtime` thread-safety defect as a side effect of moving formatting out of the
  governed record entirely, rather than patching it in place.

### Negative
- Introduces a second `LogRecord`-shaped type (or a template parameterized on capacity), which is
  more surface area to document and keep in sync than the current single struct.
- Fixed-capacity fields truncate; callers that relied on unbounded `std::string` messages must be
  updated, and truncation must be observable (a flag on the record, not silent).
- No mechanical enforcement exists yet (see Decision 5) — this ADR alone does not prevent a future
  change from re-introducing an allocation in the governed core; it only makes the boundary reviewable.

### Risks and Mitigations
- **The governed/adapter split is stated but not enforced, and drifts under later edits.**
  *Mitigation*: track the no-heap object scan as a named follow-up issue before this ADR is marked
  Accepted, mirroring MduX's own experience of stating a lint before it existed (MduX ADR-004,
  amendment of 2026-08-11).
- **Truncation is silent and looks like a smaller, successful write.** *Mitigation*: the bounded
  result type in Decision 3 must distinguish `Truncated` from `Written`, and callers on a Class C
  path should treat `Truncated` as requiring the same attention as `Dropped`.

## References
- [MduX ADR-004: Trust zones in C++](https://github.com/ambroise-leclerc/MduX/blob/main/docs/adr/ADR-004-trust-zones-in-cpp.md) — the governed/adapter split this ADR adapts.
- [MduX ADR-005: Error handling and exceptions policy](https://github.com/ambroise-leclerc/MduX/blob/main/docs/adr/ADR-005-error-handling-and-exceptions-policy.md) — the no-throw rationale for the governed zone.
- [MduX `Trace.cppm`](https://github.com/ambroise-leclerc/MduX/blob/main/include/mdux/medui/Trace.cppm) — the caller-owned ring pattern (`SampleRing`) this ADR's `RingLog` mirrors.
- [MduX `cmake/MduXNoHeapScan.cmake`](https://github.com/ambroise-leclerc/MduX/blob/main/cmake/MduXNoHeapScan.cmake) — the mechanical verification model named in Decision 5.
- mddlog issue #5 — documents the unbounded queue and the root-level `mddlog.cppm` ambiguity this ADR builds on without resolving.

## Approval
- **Decision Date**: not yet approved — drafted for review.
- **Approved By**: pending (project maintainer).
- **Review Date**: when a follow-up issue implementing this ADR's `RingLog`/`InlineString` types is opened, or when issue #5's build/test work lands, whichever is first — issue #5's own scope note ("bounded real-time queue design is outside this issue") makes this ADR's ring buffer the natural next step rather than part of that issue.
