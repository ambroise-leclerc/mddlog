# Changelog

Written for a reader deciding whether a version does what they need, and for an auditor asking what
changed between two artefacts they hold. Entries name the issue that owns the work, because the
issue carries the reasoning and this file carries the outcome. The release procedure, including when
an entry moves from `unreleased` to a date, is [`docs/release-process.md`](docs/release-process.md).

mddlog is an experiment shaped by IEC 62304, ISO 13485 and ISO 14971 concerns. No entry in this file
is a certification, a validation for medical-device use, or a production-readiness claim.

---

## Unreleased

- Add the optional Linux `FileStorageMedium` adapter, with private segment files,
  exclusive writer/shared offline reader ownership, bounded capacity and reads,
  complete short transfers, and explicit file/directory barriers (#114, part of #112).
  Durability remains `Unsupported` unless the integrator declares a qualified
  deployment. Failed mutations stop the writer and leave evidence for recovery.
- Reserve segment identities with a persistent counter before creation, including after
  reclamation and reopening. Reject substituted segment inodes, avoid FIFO blocking,
  close descriptors on exec, and distinguish inventory validation from syscall errors.
- Add common `StorageMedium` conformity checks, real file reopening and deterministic
  syscall failures, plus the standalone `FileAudit` composition example. Physical
  qualification, abrupt-stop/power-cut campaigns and epic closure remain open.

## 0.3.0 — 5 October 2026

Closes epic #113 and its four sub-issues #127–130: owned contexts and reusable bindings
for diagnostic and audit logging, designed in
[ADR-005](docs/adr/ADR-005-contextual-logging-api.md). The initial software development
file baseline (#124) is also delivered. This is an intermediate release towards 1.0;
epic #112 and the remaining 1.0 work are open.

### Contextual logging (epic #113)

- `DiagnosticContext` owns component, operation and correlation identifiers, with typed
  construction refusals and derived operation contexts. `DiagnosticBinding` reuses them
  with `SimpleLogger` and `TextLogger`, preserving the actual caller's source.
- `debugLazy(factory)` and `logLazy(level, factory)` defer message construction until
  after the adapter filter. `SimpleLogger::is(level)` exposes that filter snapshot.
- `GovernedBinding` keeps host-supplied time, bounded admission and `WriteResult` explicit.
  `AuditDescription`, `AuditContext` and `AuditBinding` prepare invariant fields while
  phase, time, detail and admission remain visible at every event.
- Five compiled before/after usages and a local stock component exercise refusal,
  temporary data, separate producers/consumers and shutdown. Source and installed
  consumers cover both public targets. See the
  [migration guide](docs/migration/contextual-logging.md) and
  [accepted local integration](docs/contextual-api-integration.md).
- The software development file records the accepted initial baseline and local API
  integration: GAP-006 closed, REQ-009/REQ-011 implemented, qualification gaps retained.
- The umbrella module exports the diagnostic refusal, ring and write-result types needed
  to use its contextual API without direct core imports. Source and installed consumers
  inspect a construction refusal and exercise governed admission and saturation.
- All three READMEs show the contextual syntax. Existing low-level APIs and the global
  `Log` facade remain available; no audit encoding or SPSC contract is changed.

### Known limits

- Local component integration is accepted; independent application validation (#122),
  budgets (#117), extended robustness (#120), persistence orchestration (#116) and the
  final API/package freeze (#121) remain open. This release is not a 1.0 qualification,
  certification or validation for a medical device.
- Bindings borrow destinations, which must outlive every binding copy. Each ring remains
  single-producer/single-consumer; the host controls stream identities, lifetimes,
  privacy and the response to a critical refusal.
- Audit categories are copied without enum-range validation, as in existing admission.
  Hosts use named values and validate external conversions. Phases are host declarations;
  admission is in-memory, and no destructor manufactures an action outcome.
- `debugLazy` is the only per-level lazy shortcut; other levels use `logLazy`.
  Filtering is a snapshot, not transactional. `TextLogger` retains the fixed
  `[component:operation:correlation]` format, including `[::]` for an empty context.
- `SimpleLogger` still has an allocating unbounded asynchronous queue; `TextLogger`
  renders synchronously with allocation. Neither is the governed path.
- Real storage and independent anchor backends are not supplied (#114/#115). Durability
  depends on the medium; tamper evidence depends on an independent anchor and retained
  position. Nothing is signed. ADR-004 implementation evidence still awaits its separate
  acceptance; this release does not reuse #113 acceptance for it.
- `import std` remains experimental and tied to the documented CMake/compiler tuple.

## 0.2.0 — 4 October 2026

Closes epics #9, #10 and #11: a regulatory audit-event model with bounded admission and explicit
hand-off ([ADR-002](docs/adr/ADR-002-regulatory-audit-event-model.md)), the building blocks for
integrating mddlog into an application ([ADR-003](docs/adr/ADR-003-application-integration-and-sink-ownership.md)),
and audit persistence with tamper evidence relative to an independent anchor
([ADR-004](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md)).

### Audit events, separate from diagnostic logging (epic #9, ADR-002)

- **Bounded admission (#56).** `AuditEvent` carries exact identifiers (action, actor, target,
  requirement and risk references, correlation), a category and a phase, a host-supplied time and a
  stream identity; only `detail` may shorten, with a flag. `AuditRing<N>` assigns a per-stream
  sequence to admitted events only and refuses rather than overwrites when full.
- **Hand-off and health (#57).** `AuditSinkAdapter` drains audit rings to an `AuditSink`, releases
  only the accepted prefix, keeps rejected events for retry and reports hand-off, failures, losses
  after admission and pending events through its own health signal.
- **Scenario validation (#59)** of categories, identifiers and MduX `ActionTrace` conversions, in
  [`docs/audit-scenario-validation.md`](docs/audit-scenario-validation.md).
- **Public API (#58).** `SimpleLogger::logAudit(AuditInput)` and `Log::logAudit(AuditInput)` return
  the ring's admission result after `setAuditRing()`.
- **Stream identities (#94).** The consumer refuses an invalid or already registered stream
  identity. ADR-002 accepted on 2026-10-02.

### Application integration (epic #10, ADR-003)

- **`SinkRegistry` (#67):** synchronized registration with explicit handles and quiescent removal,
  including self-removal from a callback.
- **`TransportConsumer` (#68):** bounded rings drained on one consumer thread, with independent
  health, failure isolation per transport, and suppression of reentrant logging feedback.
- **`TextLogger` (#69, #70):** a diagnostic text adapter with independent per-group masks, caller
  source and separate dumps, rendering through `SinkRegistry` callbacks.
- **WebFront as reference consumer (#71, #72):** adopted and verified in WebFront's own repository
  against a pinned mddlog; since #103, mddlog's tests contain and download no application code.
  ADR-003 accepted, then amended by #103.

### Audit persistence and tamper evidence (epic #11, ADR-004)

- **Design accepted at milestone A (#84 to #88):** the anchor and the reader's monotonic position,
  the canonical byte contract, storage and the meaning of "durably confirmed", retention and chain
  state recovery, and the bounds on what may be claimed.
- **Canonical serialization and chaining (#89):** a versioned canonical encoding of every audit field
  and a SHA-256 chain per stream instance, fixed by reference vectors.
- **Anchors and verification (#90):** an anchor provider interface, the reader's retained position,
  and a verifier that reports one verdict per stream (Anchored with its range, Unanchored, anchor
  unavailable, Incomplete, Altered, Rolled back, Conflict, Retired, Cannot verify, Inconsistent),
  never an unqualified "valid".
- **Storage and durable confirmation (#91):** `PersistingAuditSink` writes append-only segments
  through a `StorageMedium`, publishes a durable position only after a sync answered durable, and
  reports every storage failure through its health signal, never as an audit event.
- **Restart, rotation and retention (#92):** a ledger per adapter start, chain state recovery with a
  continuity check against the anchor, prefix rotation bounded by the anchor, whole-stream and
  ledger retention, and `LogVerifier`, which reports each stream's verdict and every boundary.
- **End-to-end validation (#93):** every validation scenario of ADR-004 Decision 5 and every
  criterion of milestone C runs from producer admission to the reader's report, in
  [`docs/audit-persistence-validation.md`](docs/audit-persistence-validation.md).

### Breaking changes

- `LogLevel::Audit` and the positional `logAudit(...)` overloads are removed; use
  `logAudit(AuditInput)` ([`docs/migration/audit-admission.md`](docs/migration/audit-admission.md)).
- Producer admission refuses every action beginning with `mddlog.`, reserved for the persistence
  ledger (`AuditRefusalReason::ReservedAction`).

### Known limits, stated because they are easy to mistake for defects

- **Not certified or validated.** Nothing here qualifies mddlog as medical-device software or
  validates it for production use.
- **Durability depends on the medium.** The library ships only `InMemoryStorageMedium`, a test
  double. Whether a real file, flash or device-log medium meets ADR-004 Decision 9, and so whether
  "durably confirmed" holds on it, is for the integrator to establish.
- **Tamper evidence is relative to an independent anchor, never tamper-proof.** The library ships
  only `InMemoryAnchorProvider`. Records past the last anchor can be altered or truncated
  undetectably; a rollback of log and anchor together is detected only with a retained position; a
  compromised provider defeats the mechanism. Nothing is signed, so nothing proves who wrote a
  record; signing, key management and an export format are deferred (ADR-004 Decision 11).
- **ADR-004's evidence awaits review.** Its design is accepted; accepting the implementation's
  evidence is a separate decision of the maintainer.
- **Single consumer.** `AuditSinkAdapter`, `TransportConsumer` and `PersistingAuditSink` belong to
  one consumer thread; ThreadSanitizer still runs the RingLog scenarios only.
- **Host obligations.** Stream identities are unique per consumer adapter and per log; across
  adapters, processes and boot sessions they remain the host's obligation. The `Lifecycle` and
  `Operator` categories have no runtime call site in a known consumer.
- **The allocating logger is unchanged.** `SimpleLogger` still uses an unbounded queue, and
  `TextLogger` renders and calls back synchronously, allocating.

## 0.1.0 — 27 September 2026

The first version, closing epic #8: a bounded, allocation-free governed logging core separated from
the allocating adapter and sinks, as accepted in [ADR-001](docs/adr/ADR-001-allocation-free-governed-logging-core.md).
It also carries the build, verification and convention work that preceded it (#5, #7).

### The governed core, `mddlog::core` (epic #8)

- **Contracts first (#31, realigned by #46).** ADR-001 fixes capacities in bytes, the memory budget,
  host-supplied time, per-field truncation rules and the admission result before any code, and was
  accepted on 2026-09-24. #46 later aligned its names and history with the implementation.
- **Two CMake targets with a one-way dependency (#32).** `mddlog::core` holds only governed modules;
  `mddlog::mddlog` links it and adds the adapter and sinks. `SimpleLogger` and the allocating
  `LogRecord` moved to `mddlog.adapter.logger` and `mddlog.adapter.logrecord`, with no compatibility
  shim under `mddlog.core.*`.
- **`InlineString<N>` (#33)**: owned, fixed-capacity, `constexpr` and `noexcept`. Identifiers are
  stored whole or refused; descriptive text is truncated without splitting a UTF-8 sequence.
- **`WriteResult` (#34)**: admission, the first refusal reason (`RingFull`, `IdentifierTooLong` with
  the offending field, `MalformedTime`) and message truncation, returned by value with an invariant
  its accessors cannot break.
- **`GovernedRecord` (#35)**: a bounded record capturing level, host-supplied `RawTime`, source
  location, message and the component/operation/correlation identifiers at emission. A refused
  assignment leaves the previous record unchanged; the core reads no clock.
- **`RingLog<Capacity>` (#36)**: a single-producer/single-consumer ring with inline storage that
  refuses new writes when full rather than overwriting, uses the two release/acquire exchanges of
  ADR-001 Decision 4, counts `RingFull` refusals atomically, and drains through one or two read-only
  spans until an explicit, validated acknowledgement.
- **Concurrency under ThreadSanitizer (#37)**: producer/consumer scenarios on separate threads, with
  per-ring ordering checks and no global-order assumption, run in a dedicated TSan CI job.
- **Adapter-zone drain (#38)**: `RingSinkAdapter` copies each drained record into an owning
  `LogRecord` before acknowledging, forwards it to existing sinks with its truncation flag and an
  explicit unavailable-time marker, and exposes per-ring refusal counts.
- **Boundary checks (#39)**: after every build with tests, CTest verifies the compiler-reported
  module imports and evaluated CMake links against a reviewed policy, scans governed sources for
  forbidden constructs, and scans governed objects for allocation and exception symbols, each with
  negative controls. [`docs/governed-evidence.md`](docs/governed-evidence.md) states what these
  checks do and do not establish.
- **Standalone consumption and migration (#40)**: the same core-only program builds and runs in-tree
  and against the installed package while linking only `mddlog::core`;
  [`docs/migration/governed-core.md`](docs/migration/governed-core.md) covers the moved imports,
  capacities, admission and host time.

### Foundations (#5, #7)

- Reproducible builds with CMake presets and a pinned toolchain matrix, SpecLab scenario tests
  discovered per scenario by CTest, and CI on Linux GCC 16.1, Linux Clang 21 with libc++, macOS
  arm64 Clang 21.1.8 and Windows MSVC, plus ASan/UBSan (#5).
- MduX conventions: `LogLevel` enumerators renamed `Trace`…`Audit`, and reproducible
  clang-format and clang-tidy checks in CI (#7).

### Known limits, stated because they are easy to mistake for defects

- **Not certified or validated.** Nothing here qualifies mddlog as medical-device software; an
  allocation-free core makes integration under strong constraints easier, it does not qualify it.
- **The allocating logger is unchanged.** `SimpleLogger` still queues records in an unbounded,
  mutex-protected queue and offers no real-time guarantee. The governed ring is a separate path
  drained by `RingSinkAdapter`; it is not wired into `SimpleLogger`.
- **`RingSinkAdapter::drainOnce()` does not isolate one ring's failure.** An exception from one
  ring's copy or acknowledgement ends the pass, so later rings are not drained in it and the count
  already delivered is not returned.
- **Sinks do not preserve every field.** `ConsoleSink` is the only implemented sink and renders a
  human-readable line; it omits `correlationId` and `complianceStandard`. `SimpleLogger::flush()`
  ignores a sink's flush failure. File, network and audit sinks, formatters and the audit model of
  ADR-002 to ADR-004 are planned (epics #9, #10, #11).
- **Governed evidence is scoped, not a proof.** The source check is lexical, the symbol scans cover
  the inspected objects and representative template instantiations only, and neither establishes
  absence of blocking or a worst-case execution time. `-fno-exceptions` is not enabled or promised
  with `import std`. ThreadSanitizer runs the RingLog scenarios only.
- **`MalformedTime` is unreachable by design.** `RawTime` has only an available and an unavailable
  state (ADR-001 Decision 7); the reason stays in the enumeration for a future time representation.
- **A file importing `mddlog.core.*` directly must link `mddlog::core`,** even when it also links
  `mddlog::mddlog`: with GCC 16.1 and CMake 4.1, modules reached only transitively are missing from
  its module mapper.
- **Toolchain window.** `import std` support is experimental and qualified only for CMake 4.0 to 4.3,
  Ninja generators, GCC 16.1+, Clang 20+ (21 in CI) and MSVC 17.14+.
