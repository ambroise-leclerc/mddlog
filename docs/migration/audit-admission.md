# Audit admission: initial #9 implementation

`mddlog.core.auditevent` and `mddlog.core.auditring` introduce a separate,
fixed-capacity audit record and a single-producer/single-consumer memory lane. They do not
yet change `SimpleLogger::logAudit()`, `Log::logAudit()`, or the diagnostic `LogLevel::Audit`.
Those legacy paths still have their old sink behavior. Callers wanting the new admission
contract must use `AuditRing` directly for now.

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
| `riskRef` | 64 | `riskLevel` if it is a stable identifier |
| `correlationId` | 117 | source stream identity plus source sequence |
| `streamId` | 96 | host-supplied identity of device, boot session, producer instance |
| `detail` | 160 | `message`; UTF-8 boundary truncation is flagged |

These limits are an initial public contract, based on examples available in this repository.
The correlation capacity covers the full 96-byte source stream identity, a `:` separator,
and all 20 decimal digits of a 64-bit source sequence. With the stated identifier grammar,
`<sourceStreamId>:<decimalSourceSequence>` is unambiguous when split at the final colon.
The host must ensure the source stream identity is unique across producer recreation and
boot sessions; mddlog cannot establish that uniqueness from string syntax alone.
They need review against deployment call sites before ADR-002 can be accepted. Longer free text
belongs in `detail`; an invalid event-field identifier is refused with
`InvalidIdentifier` and the offending `AuditField`. An invalid `streamId` returns
`InvalidStream` with `AuditField::None`, as do refusals unrelated to identifiers.
The old default `complianceStandard = "IEC_62304"` is not copied onto
every event. A meaningful per-event standard reference belongs in `requirementRef`.

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
hand-off do not survive loss of power. The legacy `logAudit()` API remains separate until
#58 migrates it.
