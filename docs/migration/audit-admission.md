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
| `correlationId` | 96 | source stream identity plus source sequence |
| `streamId` | 96 | host-supplied identity of device, boot session, producer instance |
| `detail` | 160 | `message`; UTF-8 boundary truncation is flagged |

These limits are an initial public contract, based on examples available in this repository.
They need review against deployment call sites before ADR-002 can be accepted. Longer free text
belongs in `detail`; an identifier outside its grammar or byte limit is refused with the
offending `AuditField`. The old default `complianceStandard = "IEC_62304"` is not copied onto
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
power discards the queue. Consumer failures and unacknowledged events still require the
dedicated audit health signal and adapter work in #9.

For an ActionTrace-style critical action, emit `Requested`, then `Confirmed` when a real
confirmation occurs, then `Executed` or `Failed` after the host acts. Each call gets a new
audit sequence. Keep the source sequence in `sourceSequence` only as provenance. Derive the
shared `correlationId` from both the source stream identity and source sequence; the bare
number is insufficient across producer recreation. `RawTime::unavailable()` is admitted;
timestamps may repeat or move backwards, so order within a stream is given by sequence.
