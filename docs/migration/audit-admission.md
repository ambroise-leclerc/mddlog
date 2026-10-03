# Audit admission and migration from logAudit()

`mddlog.core.auditevent` and `mddlog.core.auditring` provide a separate,
fixed-capacity audit record and a single-producer/single-consumer memory lane.
`SimpleLogger::logAudit(const AuditInput&)` and `Log::logAudit(const AuditInput&)` now
return `AuditWriteResult` from that lane. The old positional-string overloads and
`LogLevel::Audit` have been removed; old source calls fail to compile rather than
silently entering diagnostic logging. `fromString("AUDIT")` returns `nullopt`, as do
other unknown severity strings.

## Configure the public API

Create `AuditRing<Capacity>` with a host-issued identity unique to the producer
instance and boot session. Bind it using `logger.setAuditRing(ring)` or
`Log::setAuditRing(ring)`. The ring must outlive the binding, and no other producer
may write to it. Calls through one logger are serialized at admission. Call
`clearAuditRing()` before destroying the ring. Register the same ring with
`AuditSinkAdapter::addRing()`, configure an `AuditSink`, and drain it from one consumer
thread. The diagnostic console sink installed by `Log::initialize()` is unrelated.
Do not bind the same ring to two loggers (including `Log` and a separate
`SimpleLogger`): each logger serializes only its own calls, so two bindings would
violate the ring's SPSC producer contract. This exclusivity is a host obligation,
not checked at run time. `Log::shutdown()` clears the global logger's binding, even
on a retained handle from `Log::getLogger()`; after a later `Log::initialize()` or
automatic reinitialization, bind the ring again before calling `Log::logAudit()`.

`logAudit(input)` returns `Unconfigured` with `AuditField::None` when no ring is bound.
An invalid ring identity returns `InvalidStream`; a full ring returns `RingFull`.
Admission ignores diagnostic `setEnabled`, `minLevel`, and diagnostic sink filters.
Sink absence, disablement and failures are reported at hand-off by `AuditSinkAdapter`
and its health snapshot, not by the synchronous admission result.

Example admission (the host must separately configure and monitor an audit consumer):

```cpp
AuditRing<16> ring{"device_789:boot_12:ui_1"};
logger.setAuditRing(ring);
const AuditInput input{.category      = AuditCategory::Configuration,
                       .phase         = AuditPhase::Executed,
                       .action        = "CONFIG_CHANGE",
                       .actor         = "admin456",
                       .target        = "device_789",
                       .correlationId = "device_789:boot_12:input_1:41",
                       .sourceSequence = 41,
                       .detail        = "System configuration changed"};
const auto result = logger.logAudit(input);
if (!result.wasAdmitted()) {
    // Decide locally how the application responds to result.refusal().
}
logger.clearAuditRing();
```

The old positional `message` maps to `detail`, `eventType` to `action`, `userId` to
`actor`, and `deviceId` to `target` only if the device is the acted-on object; it
may instead contribute to the stream identity. `riskLevel` such as `MEDIUM` is a
classification, not a hazard reference: populate `riskRef` only with a stable
hazard or risk-control identifier. A per-event normative reference belongs in
`requirementRef`. Callers must choose the actual category, phase, time and correlation;
the old signature did not contain enough information to infer them. `Requested`
does not imply `Confirmed` or `Executed`.

## Identifier contract

The current examples pass `CONFIG_CHANGE`, `EMERGENCY_SHUTDOWN`, `admin456`,
`device_789`, and risk values such as `MEDIUM`. These fit the initial grammar:
ASCII letters, digits, `_`, `.`, `:`, `/`, and `-`. No identifier is shortened.
`action`, `target`, and `streamId` must be nonempty. Other identifiers may be empty.

| Field | Maximum bytes | Earlier `logAudit()` source |
| --- | ---: | --- |
| `action` | 64 | `eventType` |
| `actor` | 64 | `userId` |
| `target` | 96 | `deviceId`, when it is the object acted on |
| `requirementRef` | 64 | explicit caller reference |
| `riskRef` | 64 | stable hazard or risk-control identifier; not a free-form risk level |
| `correlationId` | 117 | source stream identity plus source sequence |
| `streamId` | 96 | host-supplied identity of device, boot session, producer instance |
| `detail` | 160 | `message`; UTF-8 boundary truncation is flagged |

These limits are an initial public contract, based on examples available in this repository.
The correlation capacity covers the full 96-byte source stream identity, a `:` separator,
and all 20 decimal digits of a 64-bit source sequence. With the stated identifier grammar,
`<sourceStreamId>:<decimalSourceSequence>` is unambiguous when split at the final colon.
`AuditSinkAdapter::addRing()` refuses an identity already registered on that adapter (see
below). Uniqueness across adapters, processes and boot sessions remains the host's
responsibility: mddlog cannot establish it from string syntax alone.
The external-consumer review for #9 (MduX and WebFront, see the validation report) found
no identifier exceeding these limits. Longer free text
belongs in `detail`; an invalid event-field identifier is refused with
`InvalidIdentifier` and the offending `AuditField`. An invalid `streamId` returns
`InvalidStream` with `AuditField::None`; other non-field refusals also use `None`.
The old default `complianceStandard = "IEC_62304"` is not copied onto
every event. A meaningful per-event standard reference belongs in `requirementRef`.

## Reserved actions (#92)

Actions that begin with the case-sensitive ASCII prefix `mddlog.` belong to the persistence
adapter's ledger (ADR-004 Decision 10.1). `AuditRing::tryRecord()`, and so `SimpleLogger::logAudit()`
and the `Log` facade, refuse them with `AuditRefusalReason::ReservedAction` and
`AuditField::Action`. The refusal comes after the identifier grammar is checked and before a
sequence or a slot is consumed, so the next admitted event takes the sequence it would have taken.
`mddlog` without the dot, `Mddlog.x` and `x.mddlog.y` are ordinary actions. **Breaking change:**
a producer that used an action under `mddlog.` must rename it. Nothing else about the
grammar changes, and `AuditEvent::assign()` still accepts the prefix, so that the adapter can build
its own records. `PersistingAuditSink::accept()` refuses such an event as well and counts it as
`reservedActionRefused`.

## Admission and sequence

Construct one `AuditRing<Capacity>` per producer with a unique `streamId` for that producer
instance and boot session. The host must issue a new identity after a restart or when a
producer is recreated and its counter restarts. The library cannot infer the device's or
boot session's identity. A missing or malformed identity yields `InvalidStream` at admission.

`tryRecord()` assigns sequence numbers from 1, one for each admitted event. Refusals do not
consume numbers. The 64-bit counter admits `UINT64_MAX` once, then permanently returns
`SequenceExhausted`; it never wraps to an ambiguous sequence. The ring refuses new events
when full and never overwrites admitted ones. Only `RingFull` increments `refusalCount()`.
`acknowledge()` releases the prefix consumed from the current view. Exactly one producer and
one consumer may use a ring concurrently.

The return value means **admitted to memory**, not handled by a sink or persisted. Loss of
power discards the queue. The separate consumer and health contract is described below.

For an ActionTrace-style critical action, emit `Requested`, then `Confirmed` when a real
confirmation occurs, then `Executed` or `Failed` after the host acts. Each call gets a new
audit sequence. Keep the source sequence in `sourceSequence` only as provenance. Derive the
shared `correlationId` from both the source stream identity and source sequence; the bare
number is insufficient across producer recreation. `RawTime::unavailable()` is admitted;
timestamps may repeat or move backwards, so order within a stream is given by sequence.
`sourceSequence` alone is provenance, and never substitutes for the newly assigned audit
sequence. See [the scenario evidence](../audit-scenario-validation.md) for the tested
ActionTrace conversion, field boundaries, and time and identity cases.

## Audit-only hand-off and health (#57)

`mddlog.sinks.auditsink` defines `AuditSink`, which accepts an `AuditEvent` directly and has
no diagnostic severity threshold. `mddlog.adapter.auditdrain` defines `AuditSinkAdapter`.
Register producer-owned rings with `addRing()`, configure one audit sink with `setSink()`,
and call `drainOnce()` from exactly one consumer thread. Registration and sink replacement
also belong to that thread. A diagnostic `Sink` is a different interface and cannot be
installed as an audit sink.

`addRing()` returns an `AuditRingRegistration` (#9). A ring whose identity is invalid returns
`InvalidStream`; a ring whose identity was already registered on this adapter returns
`DuplicateStream`. That includes a second concurrent producer and a recreated producer that
kept its identity while its sequence restarted. A refused ring is not drained, and each
refusal increments the configuration-error counter with that status as `lastIssue`.
Registered identities stay reserved for the adapter's lifetime. **Breaking change:** the
result is `[[nodiscard]]`; check it where the previous `void` call was ignored.

`AuditSink::accept()` returns true only when the sink has taken responsibility for the
event in memory. The adapter then acknowledges that event in its ring and increments
`handedOff`. A false return or exception leaves it queued. `drainOnce()` returns a
`SinkRejected` or `SinkThrew` status, and `healthSnapshot()` records the failed attempt,
pending ring records, and events taken for an attempt but not acknowledged. A later call
can retry. If a sink partly acted before reporting failure, it may see the same event
again; sink implementations should deduplicate by `(streamId, sequence)`.

A missing or disabled audit sink returns `MissingSink` or `DisabledSink`, increments the
configuration-error counter and leaves every event queued. The audit path does not read
`SimpleLogger::setEnabled()`, its diagnostic threshold, or diagnostic sink filters.
`healthSnapshot()` is an atomic, independently readable signal; its counters are not one
cross-thread transaction. A sink or host that discovers a loss *after hand-off* calls
`reportLoss()`. The adapter cannot detect an unreported downstream loss. A rejection that
leaves an event queued is a dispatch failure, not a loss.

Neither a true `accept()` result nor ring acknowledgement confirms durable storage.
The third level of the delivery contract remains the work of #11. In-memory admission and
hand-off do not survive loss of power.
