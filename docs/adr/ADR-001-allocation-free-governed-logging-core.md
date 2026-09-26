# ADR-001: Allocation-free governed logging core for IEC 62304 Class C

## Status
Accepted — the governed/adapter boundary and the contracts in Decisions 1–8 are the design the
codebase is expected to conform to. `GovernedRecord`, `InlineString`, `RingLog`, the adapter-zone
ring drain, the adapter boundary and its checks are implemented; acceptance alone does not establish
validation of a particular build (see Consequences and Approval).

All MduX references in this record are pinned to commit
[`d972d77`](https://github.com/ambroise-leclerc/MduX/tree/d972d77bc5cefdbe105ad7933ee61746fb5eb45b),
the same baseline mddlog issue #5 verifies against, so the comparisons stay checkable as MduX moves.

## Context

When this ADR was first drafted (2026-09-21, #6), `README.md` described mddlog as "real-time
capable" and listed "Memory pool allocation for zero-allocation logging" as `(planned)`. The module
set of that time did not deliver a governed path. The observations below describe the code at that
drafting date, in the past tense and with the paths of that time; the note after them records what
has changed since. They motivated the structural split:

- The allocating `LogRecord` (then `include/mddlog/core/LogRecord.cppm`) carried nine
  `std::string` data members (`message`, `category`, `userId`, `sessionId`, `deviceId`, `operationId`,
  `auditEventType`, `riskLevel`, `complianceStandard`) plus a
  `Metadata = std::unordered_map<std::string, std::string>`. Any of those values may allocate,
  depending on length and the implementation's small-string capacity; `addMetadata()` may
  allocate for the map node and for key/value storage. The point is not that every field allocates
  every time — it is that the type provides no bound on whether it does.
- `LogRecord::getFormattedTimestamp()` and `getSourceLocationString()` built a
  `std::stringstream` per call and formatted through `std::put_time`/`std::gmtime`. `std::gmtime`
  returns a pointer into an implementation-shared buffer with no standard thread-safety guarantee;
  calling it from concurrent producers is a data race, which directly contradicted the "thread-safe"
  claim this same type was meant to support.
- `SimpleLogger` (then `include/mddlog/core/Logger.cppm`) stored sinks as
  `std::vector<std::shared_ptr<Sink>>` (`SinkPtr`, `sinks/Sink.cppm`), queued records in an
  **unbounded** `std::queue<LogRecord>` (`logQueue`), and swallowed sink exceptions with bare
  `catch (...)` (`writeToSinks()`, `flushSinks()`) — comments in both admitted "In a medical device,
  this might need more sophisticated error handling" without providing it.
- `ConsoleSink::write()` (`sinks/ConsoleSink.cppm`) formatted through `std::ostream` and wrapped
  the whole body in `catch (const std::exception&)`.
- A second, unregistered copy of most of this (`LogRecord`, `Sink`, `ConsoleSink`, `SimpleLogger`)
  exists in the root-level `mddlog.cppm`, which `CMakeLists.txt` does not build
  (`target_sources(mddlog ... FILE_SET cxx_modules ...)` lists only `include/mddlog/mddlog.cppm` and
  its submodules). GitHub issue #5 already flags this as "the unused root-level `mddlog.cppm`
  ambiguity" to resolve; this ADR does not resolve it, but the allocation-free core defined below
  should not be duplicated into it once it is either deleted or reconciled.
- Issue #5 separately documents the unbounded queue as an accepted limitation ("Document the
  existing unbounded queue as a limitation; bounded real-time queue design is outside this issue")
  and treats "persistent/cryptographically protected audit storage" as out of its own scope. The
  queue half is what this ADR closes; the audit record and its delivery contract are ADR-002's, and
  persistent/tamper-evident storage is ADR-004's.

**Since the draft.** Several of these observations no longer describe the code. Before this ADR
was accepted, #12 (2026-09-22) replaced `std::gmtime` in `getFormattedTimestamp()` with
`std::format` over the time point (only `getSourceLocationString()` still uses a
`std::stringstream`), and made `writeToSinks()` record each caught sink failure through the sink's
`recordWriteFailure()` statistic; `flushSinks()` still ignores flush failures. #32 moved
`LogRecord` and `SimpleLogger` to `mddlog.adapter.logrecord` and `mddlog.adapter.logger`, and #38
added a tenth `std::string` member, `correlationId`, for governed records drained through the
adapter. The allocating logger and its unbounded queue still coexist with the governed path; the
remaining observations stand.

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
  that defers the failure to an unpredictable later moment, as `logQueue` does today.
- **Bounded time is a separate property from bounded memory.** An allocation-free call is not
  automatically a time-bounded one. The governed path must therefore also exclude unbounded retry
  loops and blocking waits, and Decision 6's verification criteria must not be read as covering
  timing merely because they cover allocation.

### Risk management considerations
- A silently swallowed sink exception (`catch (...)` with no counter, no re-raise, no side channel)
  is a hazard in its own right for the audit-relevant levels: a caller believes an `Audit` or
  `Error` record was recorded when it may not have reached any sink at all. ADR-002 addresses the
  audit path specifically; the governed core defined here must not add a *second* unreported failure
  mode of its own. Note the split, because Decision 3 alone cannot carry it: a **refusal** is
  synchronous and is reported by the call's own result, while a loss **after** admission — a sink
  that fails once the record has already been accepted — is outside that result by construction and
  is reported through ADR-002 Decision 3's asynchronous audit-health signal. Neither may be
  swallowed; they are simply not reported by the same mechanism.

## Decision

Split mddlog into two zones, following the shape of
[MduX ADR-004](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/ADR-004-trust-zones-in-cpp.md),
adapted to a standalone library with no `mdux.governance`/`mdux.evidence` to import:

| Zone | Module(s) | May allocate / throw / block | Public API may contain |
|---|---|---|---|
| Governed core | `mddlog.core.record`, `mddlog.core.ring` | No | `std::span`, fixed-capacity value types, `std::string_view` into caller-owned storage |
| Sinks / adapter | `mddlog.sinks.*`, `mddlog.adapter.logger` | Yes | `std::string`, `std::shared_ptr`, `std::ostream`, any allocating container |

### 1. `GovernedRecord` is a fixed-capacity, non-allocating value type

The governed `mddlog::core::GovernedRecord` is separate from the historical, allocating
`mddlog::core::LogRecord`, which remains available through `mddlog.adapter.logrecord`. Its bounded
text fields use an inline buffer plus length, namely
`InlineString<N>` (a `std::array<char, N>` and a `std::uint16_t` length, `constexpr`, `noexcept`).
Capacities are declared in **bytes**, not code points; a value is accepted only if it fits whole, so
no field ever stores a partial UTF-8 sequence. `N` is constrained at compile time to the range the
`std::uint16_t` length can represent, i.e. `N ∈ [0, 65535]`, enforced by the
`InlineString` template constraint.
None of the capacities chosen below come close to that ceiling; it bounds the type, it does not
drive these values.

**Field set, capacities and parametrization (the "Standard" preset).** The governed record keeps
exactly the fields Decision 1 already names — `level`, the raw time value (Decision 7), the emission
`std::source_location`, `message`, and the three-part context envelope `component`/`operationId`/
`correlationId` — plus the message-truncation flag required by Decision 2 for sink rendering.
No other field of the current allocating `LogRecord` survives into the governed type (Decision 6
covers what happens to the rest). Capacities:

| Field | Kind (Decision 2) | Capacity | Rationale |
|---|---|---|---|
| `message` | descriptive | 160 bytes | A first documented default for a diagnostic line; under-sizing only costs truncation (never a refusal), so the risk of guessing without call-site data is lower than for an identifier field. |
| `component` | identifier | 32 bytes | Fits a module/service/component name (e.g. `mddlog.core.ring`, `webfront.session`) with margin; identifiers refuse rather than truncate, so this must not be tight against real names. |
| `operationId` | identifier | 32 bytes | Same margin reasoning as `component`; sized for a short operation/call tag, not a free-form description. |
| `correlationId` | identifier | 40 bytes | Sized to hold a 36-character canonical UUID (the most common correlation-id shape) plus 4 bytes of headroom for a non-UUID scheme, rather than the tight 36. |

**Parametrization mode.** These four capacities are `constexpr std::size_t` constants declared
alongside `mddlog.core.record`, forming one fixed preset — not template parameters exposed on the
public `GovernedRecord` type itself. `InlineString<N>` stays a reusable template, but the record type
built from it is a single, concrete (non-template) class. This is a deliberate choice between the
two options the issue that requested this section raised: exposing `N` as template parameters on
`GovernedRecord` would let every consumer pick its own capacities, but it would also mean `RingLog`, the
sinks, and the adapter would each have to agree on (or template over) the same instantiation, and a
mismatched pair would fail to link rather than fail a documented contract. One named preset avoids
that combinatorial surface. If a second footprint is ever needed (e.g. a more constrained target), it
is a **second, distinctly named** governed type — e.g. `CompactGovernedRecord` with its own constants — not a
second instantiation of the same template family.

Keep the allocating `LogRecord`'s text formatting (`getFormattedTimestamp()`, now `std::format`;
`getSourceLocationString()`, a `std::stringstream`; formerly also `std::gmtime`, see Context) out of
the governed type: a governed record stores a raw time value supplied by the host (ADR-002
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

**Exhaustive classification for the "Standard" preset (Decision 1).** Identifier-kind:
`component`, `operationId`, `correlationId` — three fields, all refuse-on-overflow, never truncated.
Descriptive: `message` — the only field currently subject to truncation. `level` and the raw time
value are fixed-size (Decision 7) and are not classified at all: classification only applies to the
variable-length, caller-supplied text fields.

**Truncation never cuts a UTF-8 sequence — for well-formed input — and does not validate
well-formedness.** When `message` must be shortened to fit its capacity, the cut point is walked
backward from the byte-`N` boundary, capped at 3 bytes of backtracking (the longest UTF-8 sequence is
4 bytes), stopping as soon as it lands on a byte that is not a UTF-8 continuation byte (top two bits
`10`). For **well-formed** UTF-8 input, that cap is never actually hit before a lead byte is found,
so the guarantee "never cuts a sequence" holds unconditionally. This is a *boundary search*, not a
validity check: it finds where a sequence starts, it does not confirm the sequence is well-formed.

For input that is **already malformed** before truncation — review correctly flagged that the earlier
wording left this case's outcome undefined, specifically four or more consecutive continuation bytes
straddling the cut point, which the 3-byte cap cannot walk past — the rule is stated explicitly rather
than left implicit: the search always stops at the cap, after at most 3 bytes of backtracking,
**whether or not** it has landed on a non-continuation byte, and the cut is made there. The governed
core neither detects nor repairs pre-existing malformed UTF-8 — the bytes are stored (truncated at
the capped boundary, or stored whole if they fit) exactly as given, with no additional flag, and the
result for already-malformed input may itself still not be well-formed UTF-8. The "never cuts a
sequence" guarantee is therefore scoped to well-formed input by construction; it was never meant to
extend to input that was malformed before this rule ever ran, and general UTF-8 well-formedness
validation remains an explicit non-goal of the governed write path — it would add a second,
content-dependent rejection or flagging mechanism this ADR does not otherwise define, and
validating/repairing text is a sink- or caller-zone concern if it is wanted at all. Identifier fields
have no equivalent complication: they are stored whole or not at all, so no boundary search ever
applies to them.

### 3. The result type expresses admission and truncation independently

A `bool` cannot carry this contract. The governed write returns a small value with two independent
components:

- **admission**: `Written` | `Refused(reason)` — where `reason` distinguishes at minimum
  `RingFull`, `IdentifierTooLong(field)` and `MalformedTime`;
- **truncation**: whether any descriptive field was shortened, and which.

**Logical shape.** The two components are independent, but only one of them is meaningful at a time in
the sense that a refused write carries no truncation (it was never stored):

```text
enum class RefusalReason : std::uint8_t { RingFull, IdentifierTooLong, MalformedTime };
enum class IdentifierField : std::uint8_t { Component, OperationId, CorrelationId };

struct Refusal {
    RefusalReason   reason = RefusalReason::RingFull;
    IdentifierField field = IdentifierField::Component;  // meaningful only when reason == IdentifierTooLong
};

enum class Admission : std::uint8_t { Written, Refused };

struct TruncatedFields {
    bool message = false;  // the only descriptive field today; grows if more are added
};

struct WriteResult {
    Admission       admission;
    Refusal         refusal;     // valid only when admission == Refused
    TruncatedFields truncated;   // valid only when admission == Written
};
```

This sketch names the logical fields, not a mutable public layout. The concrete `WriteResult` keeps
its state private: `admission()` derives the state from the optional refusal, `refusal()` returns
that refusal (absent for a written record), and `truncated()` returns the flags by value (all false
for a refusal).
Its factories establish the invariant, and callers cannot alter one component independently after
construction. A default-constructed `Refusal` initializes both members, including `field` even
when the reason is not `IdentifierTooLong`.

**Priority when several refusal conditions apply at once.** Checks run in one fixed order and the
**first** failing check is the reason reported — reasons are never accumulated into a set:

1. `MalformedTime` (Decision 7) — checked first in principle, as the cheapest local check. Under the
   `RawTime` type Decision 7 settles on, this check is **unreachable**: "no usable clock" is the
   `Unavailable` state, which is admitted and written (Decision 7), not refused, so no value of
   `RawTime` actually produces this reason today. It stays first in the ordering and in the `Refusal`
   enum (Decision 3) for the future, more constrained representation Decision 7 names as its
   attachment point.
2. `IdentifierTooLong`, evaluated in field declaration order — `component`, then `operationId`, then
   `correlationId` — so the first offending field is the one reported; a caller that fixes it and
   resubmits will see the next offending field, if any, rather than a stale reason.
3. `RingFull`, checked **last**. This is a deliberate ordering, not an arbitrary one: content
   validation (steps 1–2) happens before the write ever attempts to reserve a ring slot, so a
   content-invalid record never touches the ring, never advances the write cursor, and never
   competes with a valid write for capacity. Checking the atomic ring state is also the most
   contended of the three checks, and there is no reason to pay for it before a cheaper, purely local
   check has already decided the outcome.

Truncation is orthogonal to this ordering: it is computed for descriptive fields only after a write
is determined to be admissible, so it never participates in the refusal-priority decision above.

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
  lock-free **on the configurations this contract targets** (see the counter-width paragraph below for
  what that requires) without making cross-ring ordering a promise (ADR-002 Decision 5 covers
  ordering).
- **Cursors**: a write cursor and a **read cursor**, both monotonically increasing sequence counters
  rather than wrapped indices, so "empty", "full" and current occupancy are derivable by subtraction
  and wrap-around is not ambiguous. Refusals are **not** derivable this way: under refuse-new a
  rejected write does not advance the write cursor, so the difference measures unread occupancy and
  nothing else. Refused writes are counted separately (see Overflow).
- **Publication — two release/acquire exchanges, not one.** The first orders the producer's writes
  before the consumer's reads; the second orders the consumer's reads before the producer's reuse of
  the same storage. Both are required, and naming only the first is the mistake this bullet
  previously made:
  1. the producer fills a slot, then publishes the write cursor with a **release** store;
  2. the consumer **acquire**-loads the write cursor before reading any published slot;
  3. after its last read or copy out of those slots, the consumer publishes the read cursor with a
     **release** store;
  4. the producer **acquire**-loads the read cursor before rewriting any freed slot.

  Step 3 is what makes step 4 safe: without it there is no happens-before edge from the consumer's
  reads to the producer's next writes, and the non-atomic slot contents are racy however carefully
  the cursors are handled. Note that a `store(memory_order_acquire)` is not valid, and an
  acquire-only read-modify-write would not publish the consumer's preceding reads — step 3 must
  carry release semantics. A slot between publication and release is never rewritten.
- **Overflow**: **refuse-new**, not drop-oldest. A ring with no reusable slot refuses the write with
  `Refused(RingFull)` and increments a refusal counter. That counter is written by the producer and
  read by an observer on another thread, so it is not covered by the cursor protocol above and needs
  its own rule: an atomic counter (relaxed increment is sufficient — it is a statistic, not a
  synchronisation point), or a snapshot published alongside the write cursor. A plain `std::size_t`
  incremented by the producer and read by a monitor is a data race. Refusing a new record keeps
  every already-admitted record intact, which is the behavior ADR-002's audit lane requires; the
  earlier draft left drop-oldest open as an option, and this revision closes it, because an audit
  event that was accepted and then silently overwritten is strictly worse than one that was never
  accepted.

**Counter type, width and overflow.** The write cursor, the read cursor, and the refusal counter are
all `std::atomic<std::uint64_t>`, using ordinary unsigned wraparound (modulo 2⁶⁴) rather than
saturating arithmetic — unsigned overflow is well-defined by the standard as modulo-2ᴺ, not UB, so
"defined" does not require extra branches on the increment's hot path. **The standard does not
guarantee `std::atomic<std::uint64_t>` is lock-free on every implementation** — review correctly
flagged that the lock-free claim above needs a stated condition, not an assumption. This ADR requires
`std::atomic<std::uint64_t>::is_always_lock_free` to hold on every configuration this governed core
targets, checked by `RingLog`'s `static_assert` (the same enforcement pattern as
Decision 1's capacity bound); a target where it does not hold is not a target this ADR's "no
blocking" property covers, and Decision 6's CMake configuration should reject or flag it rather than
silently link a lock-based fallback into code labeled governed. Wraparound is treated as
**unreachable in practice, not merely defined**: at an optimistic sustained rate of 10⁸ records per
second — far beyond any plausible logging throughput — a 64-bit counter takes on the order of 5,800
years to wrap. No wrap-handling logic is implemented; the width is chosen specifically so that none
is needed within any device's operational lifetime. This same width and rule (64-bit, monotonic,
natural modulo wraparound, unreachable in practice) is what ADR-002 Decision 5 asks the implementing
issue to state for `AuditEvent.sequence`, and this is that statement.
- **Drain view lifetime**: `RingLog<Capacity>::drain()` returns a `DrainView` whose `first()` and
  `second()` expose **one or two** `std::span<const GovernedRecord>` (two when the unread region
  wraps). The consumer must finish reading or copying before calling
  `acknowledge(view, count)`; a positive acknowledgement invalidates both spans. Nothing outside
  that window may retain them.

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
  (`sinks/ConsoleSink.cppm`) emits `userId`, `deviceId`, `operationId`, `auditEventType` and
  `riskLevel`, but not `complianceStandard`, and its output is a human-readable line, not a
  round-trippable encoding. Nothing in this ADR should be read as implying that routing a governed
  record through today's sinks preserves it; the sink responsible for complete audit-field
  preservation is ADR-004's to specify.

### 6. The boundary is materialised as modules *and* CMake targets

At the decision date, `SimpleLogger` lived in `mddlog.core.logger` and imported
`mddlog.sinks.sink`, while `CMakeLists.txt` built one `mddlog` target. The following split has since
been implemented:

- The governed modules move under names that only contain governed code (`mddlog.core.record`,
  `mddlog.core.ring`). `SimpleLogger` moves out of the `mddlog.core.*` namespace to an adapter
  module — a rename, not a rewrite. **A module rename is a breaking change for anyone writing
  `import mddlog.core.logger` directly**, which keeping the class intact does not soften: the class
  survives, the import path does not. This record chooses the migration rather than a permanent
  compatibility module, because a `mddlog.core.logger` that re-exports an adapter type would
  reintroduce exactly the "core means governed" ambiguity Decision 6 exists to remove. Consumers
  that import the umbrella `mddlog` module are unaffected; direct importers of the submodule change
  one line. If that trade turns out to be wrong for a real consumer, the alternative is a
  deprecated-but-present shim module, and it should be decided deliberately rather than by
  accident.
- Two CMake targets with a **one-way** dependency: `mddlog-core` (governed, no sink modules) and
  `mddlog` (adapter, links `mddlog-core`). A consumer — MduX being the motivating one — must be able
  to import and link the governed core without pulling in `SimpleLogger`, the sinks, or their
  dependencies.

**Resolved: placement of the two existing modules this decision otherwise leaves ambiguous.**

- `mddlog.core.loglevel` **splits**. The `LogLevel` enum and its `constexpr noexcept` semantic
  helpers (`toString`, `fromString`, `isComplianceLevel`) allocate nothing, throw nothing, and are
  usable identically by governed and adapter code — they stay under `mddlog.core.loglevel` and count
  as governed. `getColorCode`/`getResetColorCode`, however, are ANSI console-styling helpers with
  exactly one caller (`ConsoleSink::write()`, `sinks/ConsoleSink.cppm`) — ANSI escape
  sequences are a presentation concern, not a governed one, so they move out of `mddlog.core.*` into
  the sink zone (inlined into `mddlog.sinks.consolesink`, since no other consumer justifies a
  separate colors module). Being allocation-free is necessary for a symbol to stay under
  `mddlog.core.*`, but this split shows it is not sufficient on its own — semantic ownership (is this
  governed-record logic, or sink presentation?) is the other half of the test.
- The allocating `LogRecord` (formerly `include/mddlog/core/LogRecord.cppm`, now
  `include/mddlog/adapter/LogRecord.cppm`) **cannot** remain under `mddlog.core.*`, per this
  decision's own rule. It moved wholesale — together with `LogStatistics` — to
  `mddlog.adapter.logrecord`. `LogStatistics` is
  mechanically allocation-free (only atomics), but it is not a governed *record* type: it counts
  outcomes of sink I/O (`bytesWritten`, `flushCount`, `totalWriteTimeNs`), which is adapter-zone
  activity by the same semantic-ownership test just applied to the color helpers, so it moves with
  the type it instruments rather than staying behind for being technically allocation-free. This
  frees the name `mddlog.core.record` (Decision 1's table) to mean only the new governed type,
  avoiding any naming collision or "core used to mean something else" ambiguity between the old and
  new records. This move is the module rename this decision already commits to for `SimpleLogger`;
  it is not a second, separately-decided rename — it is scoped here so the target-separation
  implementation issue has an unambiguous source module list to start from.

Verification criteria for that boundary, in decreasing strength: a link/import dependency-graph
check; an object scan over the compiled governed target; and source-level checks for forbidden
constructs. Issue #39 implements these as separate CTest gates on issue #5's CI infrastructure;
see the [governed evidence note](../governed-evidence.md) for the exact scope, negative controls,
toolchain coverage and limitations. A configured or unexecuted check is not a passing result.
Three limits apply:

- an allocation scan shows no listed allocator reference in the inspected objects; it is not a
  whole-program absence-of-allocation proof and shows nothing about blocking or bounded time;
- MduX's two scan profiles are not interchangeable: `ml-noheap` is the strict no-allocation profile
  over specific objects, while `governed-throw` checks for throws with tooling-dependent limits and
  does **not** forbid allocation
  ([`MduXNoHeapScan.cmake`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/cmake/MduXNoHeapScan.cmake));
- a `-fno-exceptions` build must not be promised from the shape of these types. MduX ADR-005 records
  that `import std` and `-fno-exceptions` are mutually exclusive on GCC because the dialect is
  recorded in the module BMI — mddlog uses `import std` too, so the same constraint applies here
  until demonstrated otherwise on a specific toolchain.

### 7. Host-supplied time: epoch, representation, and the unavailable-time state

Two problems in an earlier revision of this section are fixed here, both caught in review before
merge: a plain `duration` has no epoch, so nothing told the adapter what "UTC ISO-8601" was measured
from; and treating "no clock available" as a refusal reason contradicted ADR-002 Decision 5, which
requires that state to be *representable*, i.e. an audit event must still be admitted when the host
has no usable clock, not lost because of it.

- **Type — a UTC time-point, not a bare duration.** `RawTime` wraps
  `std::chrono::sys_time<std::chrono::nanoseconds>` (`std::chrono::time_point<std::chrono::system_clock,
  std::chrono::nanoseconds>`), which the standard defines as Unix Time: a count of nanoseconds since
  **1970-01-01T00:00:00Z UTC**, not counting leap seconds. That is the epoch ADR-002 Decision 5's
  "single spelling, fixed fractional-second width, UTC" rendering is measured from — a plain
  `std::chrono::nanoseconds` duration, used in an earlier draft of this section, cannot say that,
  which is exactly what review flagged. Using `sys_time` fixes the epoch without requiring the
  governed core to call anything: a `time_point` is still just a wrapped integer the caller
  constructs and passes in, not an invitation to query a clock.
- **`RawTime` is a small tagged type, not the bare time-point alone**, because "no usable clock" must
  be storable rather than refused (see below). Review correctly flagged that a public aggregate with a
  defaulted tag lets a caller construct a third, invalid state — e.g.
  `RawTime{{}, static_cast<TimeAvailability>(255)}` — which would make the "only two well-formed
  states" claim below false. `RawTime` is therefore **not** an aggregate: its members are private, and
  the only ways to obtain one are the two factories, so no caller-reachable path produces a value
  outside `Available`/`Unavailable`:

  ```text
  enum class TimeAvailability : std::uint8_t { Available, Unavailable };

  class RawTime {
  public:
      static constexpr RawTime available(std::chrono::sys_time<std::chrono::nanoseconds> v) noexcept {
          return RawTime{v, TimeAvailability::Available};
      }
      static constexpr RawTime unavailable() noexcept {
          return RawTime{{}, TimeAvailability::Unavailable};
      }

      constexpr TimeAvailability availability() const noexcept { return tag; }
      constexpr std::chrono::sys_time<std::chrono::nanoseconds> value() const noexcept { return v; }  // meaningful only when availability() == Available

  private:
      constexpr RawTime(std::chrono::sys_time<std::chrono::nanoseconds> v, TimeAvailability tag) noexcept : v(v), tag(tag) {}
      std::chrono::sys_time<std::chrono::nanoseconds> v;
      TimeAvailability                                 tag;
  };
  ```

  Trivially copyable, `constexpr`-constructible, no allocation — the same governed-type discipline as
  `InlineString`. The private constructor is the enforcement mechanism: there is no public constructor,
  brace-initializer, or setter that can assign `tag` a value outside the two the factories produce, so
  the invariant below holds by construction rather than by convention.
- **The core reads no clock.** The value is a mandatory parameter the caller supplies at the write
  call site; nothing in `mddlog.core.record` or `mddlog.core.ring` calls
  `std::chrono::system_clock::now()`, `std::gmtime`, or any other clock/time-zone API — that path was
  identified as a defect in the drafting-era `LogRecord::getFormattedTimestamp()` (Context; since
  replaced by `std::format`) and Decision 1 excludes such formatting from the governed type. `sys_time` names an epoch, it does not read one: the
  host converts whatever clock it has (a hardware RTC, an NTP-disciplined counter, or nothing at all)
  to Unix-time nanoseconds itself, or passes `RawTime::unavailable()`. This is now a stated, checkable
  constraint: the governed module's translation units should contain zero references to clock-query
  APIs, verifiable by the same source-level scan Decision 6 names for allocation and throw.
- **Unavailable time is *admitted*, never refused.** A record built with `RawTime::unavailable()` is
  written normally (`Admission::Written`); the adapter renders its time as an explicit
  "unavailable/unreliable" marker at serialization, per ADR-002 Decision 5, rather than as a zero or
  epoch timestamp that would misrepresent it as real. An audit event is not held hostage to its
  producer's clock: losing a hazard-relevant event because the clock was unavailable would be exactly
  the silent-loss failure mode ADR-002's Medical Device Considerations section rejects. This
  supersedes the earlier draft's `Refused(MalformedTime)` treatment of "no usable time," which was
  irreconcilable with ADR-002 Decision 5 as review noted.
- **`MalformedTime` stays in the `Refusal` enum (Decision 3) but is unreachable under this concrete
  type, and that is enforced, not merely stated.** `RawTime` here has exactly two well-formed states —
  `available(value)` and `unavailable()` — and, because the constructor is private, those two factories
  are the **only** way to produce a `RawTime` at all; there is no third, ill-formed tag value a
  conforming caller can construct (unlike the public-aggregate version review flagged, where
  `static_cast`ing an arbitrary integer into the tag field was possible). The core does not range-check
  `value()` for plausibility either (Decision 3 as originally drafted already disclaims that). The
  reason is kept in the enum for API stability and as the attachment point for a
  future, more constrained time representation (e.g. a validated wire format) that could actually
  produce it — the same "defined but practically unreachable" treatment Decision 4 gives counter
  overflow, applied here to a refusal reason instead of a wraparound.

### 8. Memory budget: a worked example, and what it does and does not guarantee

**What this budget commits the implementation to.** `GovernedRecord` and `RingLog` now exist.
`sizeof(GovernedRecord)` is a compile-time constant; the implementation checks it against
`maximumStandardRecordBytes` (384) with a `static_assert`. `RingLog<Capacity>` contains exactly
`Capacity` inline `GovernedRecord` slots and three atomic 64-bit counters. Its footprint is
`Capacity * sizeof(GovernedRecord)` plus fixed counter storage and any ABI padding, and cannot
grow after construction. The separate adapter's `std::queue<LogRecord>` has no such bound.

**Record layout, "Standard" preset (Decision 1).** `sizeof(InlineString<N>)` is `N` (the byte
buffer) plus 2 (the `std::uint16_t` length), rounded up to the type's alignment — a fixed cost of
`N + 2` plus at most a few bytes of padding under any reasonable compiler layout:

| Member | Approx. size |
|---|---|
| `level` (`LogLevel`) | 1 byte |
| `time` (`RawTime`, Decision 7 — an 8-byte `sys_time<nanoseconds>` plus a 1-byte `TimeAvailability` tag, padded to its 8-byte alignment) | 16 bytes |
| `location` (`std::source_location`) | ~24 bytes (implementation-defined; commonly two pointers plus two 32-bit ints on a 64-bit target) |
| `message` (`InlineString<160>`) | 162 bytes |
| `component` (`InlineString<32>`) | 34 bytes |
| `operationId` (`InlineString<32>`) | 34 bytes |
| `correlationId` (`InlineString<40>`) | 42 bytes |
| `truncated.message` (`bool`) | 1 byte |
| **Sum before alignment** | **≈314 bytes** |
| **`sizeof(GovernedRecord)`, rounded to 8-byte alignment** | **≈320 bytes** |

**`RingLog` budget, worked example.** Overhead beyond the slots is three `std::atomic<std::uint64_t>`
values (write cursor, read cursor, refusal counter — Decision 4): approximately 24 bytes on a
64-bit target, plus possible padding. A `RingLog<1024>` is therefore approximately
`1024 × 320 + 24 ≈ 327,704 bytes`, i.e. **≈320 KiB** on the illustrative ABI — a static,
computable footprint in place of "however large the queue happens to grow." The exact value for a
target is `sizeof(RingLog<1024>)`.

**What is *not* guaranteed:**
- The exact byte counts above are **not** portable across compilers/ABIs — `std::source_location`'s
  layout, struct padding, and enum packing are all implementation-defined. The guarantee is that the
  number is a fixed compile-time constant *for a given toolchain*, not that the constant is the same
  number everywhere.
- **No cache-line placement or false-sharing avoidance is promised** between the write cursor, read
  cursor and refusal counter; three atomics may share a cache line under a naive layout. Avoiding
  that (e.g. with `alignas`) is a performance optimization a later revision may add — it is not a
  correctness property this ADR requires, so it is out of scope here.
- **This budget is per `RingLog` instance.** A multi-producer deployment (Decision 4: one ring per
  producer, aggregated by the adapter) multiplies it by the number of producer rings; sizing that
  aggregate is an adapter-zone decision, not something this record fixes.

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
- Formatting stays outside the implemented `GovernedRecord` path. The `std::gmtime` data race noted
  in Context was removed from the allocating `LogRecord::getFormattedTimestamp()` by #12, which now
  formats through `std::format`; that fix is independent of this ADR.

### Negative
- Introduces the separate `GovernedRecord` alongside the allocating `LogRecord`, adding a second
  record contract to document and maintain.
- Fixed capacities make some inputs refusable that are accepted today (Decision 2), which is a
  behavioral change for existing callers, not only an internal one.
- The module rename and target split (Decision 6) are a breaking change for any consumer importing
  `mddlog.core.logger` directly — the type survives the move, the import path does not, and this
  record chooses migration over a permanent compatibility module. Decision 6 now also moves
  `mddlog.core.logrecord` (the current allocating type, plus `LogStatistics`) and part of
  `mddlog.core.loglevel` (`getColorCode`/`getResetColorCode`) into the adapter/sink zone: the
  breaking surface for direct-module importers is therefore slightly larger than the first draft
  implied, not limited to `SimpleLogger`.
- Boundary checks are implemented by #39; their scope and limitations are documented in the
  [governed evidence note](../governed-evidence.md).
- Chosen capacities (Decision 1) are a first documented default, not derived from real call-site
  measurements — the same caveat ADR-002 Decision 1 states for its own identifier grammars. An
  under-sized identifier capacity produces refusals in the field; widening a capacity later is a
  binary-compatibility-breaking change to `GovernedRecord`'s layout, not a silent fix.

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
  dependency-graph, source and object-scan checks in Decision 6 are implemented by #39 with
  negative controls and [scoped evidence](../governed-evidence.md); acceptance alone does not
  establish that any particular build executed them —
  MduX's own ADR-004 had to be amended in 2026-08 because it asserted a lint that did not yet exist,
  which is the mistake this mitigation is written to avoid repeating.
- **Refuse-new starves a slow consumer's producer under sustained load.** *Mitigation*: refusals are
  counted and observable (Decision 3), so sustained refusal is a visible condition the host can act
  on, rather than a silent one — but the host, not the logger, decides what to do about it.
- **A host that does not know its own clock's reliability leaves `time` ambiguous with a real
  timestamp, or its record is lost for lack of one.** *Mitigation*: Decision 7's `RawTime` carries an
  explicit `TimeAvailability::Unavailable` state, distinguishable from every legitimate timestamp and
  from zero/epoch; a host with no usable clock passes `RawTime::unavailable()` and the record is
  still admitted and written — never refused for lacking a clock reading, which is what ADR-002
  Decision 5 requires and an earlier draft of Decision 7 got wrong.

## References
All MduX links pinned to `d972d77bc5cefdbe105ad7933ee61746fb5eb45b`.
- [MduX ADR-004: Trust zones in C++](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/ADR-004-trust-zones-in-cpp.md) — the governed/adapter split this ADR adapts.
- [MduX ADR-005: Error handling and exceptions policy](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/ADR-005-error-handling-and-exceptions-policy.md) — the no-throw rationale, and the `import std`/`-fno-exceptions` limit cited in Decision 6.
- [MduX `Trace.cppm`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/include/mdux/medui/Trace.cppm) — `SampleRing`, cited in Decision 4 for what it is *not* a precedent for.
- [MduX `cmake/MduXNoHeapScan.cmake`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/cmake/MduXNoHeapScan.cmake) — the two scan profiles and their different scopes.
- mddlog issue #5 — the unbounded queue, the root-level `mddlog.cppm` ambiguity, and the absent CI this ADR's Decision 6 depends on.
- mddlog issue #31 — closed the open points this record left implicit (Decisions 1's capacities and parametrization, 2's UTF-8 truncation boundary, 3's exact result shape and refusal priority, 4's counter width, 6's placement of `mddlog.core.loglevel`/`mddlog.core.logrecord`, and the new Decisions 7–8 on time and memory budget), and the maintainer accepted this ADR while closing it — see Approval.
- ADR-002 (this repository) — the audit delivery contract built on Decision 3 and Decision 4, and the audit-health signal that reports losses occurring after admission.
- ADR-004 (this repository) — persistence and tamper evidence, including the sink that would have to preserve every audit field.

## Approval
- **Decision Date**: 2026-09-24.
- **Approved By**: ambroise-leclerc (project maintainer).
- **Review Date**: 2026-09-26 — revisited after `InlineString` (#33), `WriteResult` (#34),
  `GovernedRecord` (#35), `RingLog` (#36), its ThreadSanitizer tests (#37), the adapter-zone ring
  drain (#38), and the boundary checks (#39) landed. This revision aligns implementation
  names, examples, and historical status; it makes no new normative decision. Review again if
  capacities, record layout, or the drain contract change.
