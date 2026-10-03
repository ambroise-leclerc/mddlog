# Restart, rotation and retention of audit chains (#92)

This note is for a host that already persists audit events through `PersistingAuditSink` (#91)
and now wants the restart, rotation and retention behaviour of
[ADR-004 Decision 10](../adr/ADR-004-audit-persistence-and-tamper-evidence.md). It describes what
the code does. It does not make any claim about conformity: the library supplies evidence, and
what that evidence is worth is bounded by the anchors it is checked against (ADR-004 Decision 1).

## Turn the ledger on

Everything below needs a ledger. Set `StorageConfig::ledger` to a `LedgerConfig` holding a
**new stream identity for each start** (ADR-002 Decision 5), and optionally a clock for the
ledger's records. Without it the sink behaves as in #91: it stores records and writes no ledger
record, and `trimPrefix()`, `removeStream()` and `relieve()` refuse with `NoLedger`.
`StorageConfig::provider` is the anchor provider; without one nothing is anchored or retired and
rotation is not bounded by an anchor (ADR-004 10.4).

`create()` then recovers chain state and writes the ledger's record 1 (`origin`, or
`predecessor` citing the newest earlier ledger) and one `recovered` record per stream that ledger
opened and the log still holds. Each record is durably confirmed before the next is written.
`restart()` says what was written. A restart never continues an old stream: producers must use new
stream identities, and their sequences begin at 1 again. A reader reports the old and the new
instances separately, so a discontinuity is never presented as continuity.

Inconsistent state is written in its `Failed` form, citing the last position that checks, and
reported through `health().integrity`. Records are never repaired, rewritten or reordered, and
retention never removes records of a stream recovery found inconsistent.

## Closing

`closeStream()` confirms the stream's last record, attempts the anchor (7.3), then writes the
ledger's `mddlog.stream.close`. `close()` closes every stream and writes `mddlog.ledger.close` only
if all of them closed, then attempts to anchor the ledger. After an abrupt end no `close` exists,
and a reader says "ended without close at `m`". Records after `m` may have been lost before
durable confirmation or removed: the two cannot be told apart.

## Rotation and retention

| Call | What it does |
| --- | --- |
| `trimPrefix(stream)` | Removes a prefix of whole segments, up to the stream's accepted anchor, never the open or last segment. |
| `removeStream(stream)` | Removes a whole ended stream, or a whole earlier ledger that 10.5 allows, then retires its anchor. |
| `relieve()` | For a full producer stream, applies `StorageConfig::retention`: `rotate`, then `removeEnded`. Does nothing by default. |

The trim record (`mddlog.stream.trim`, with `q` and `H_q`) is durably confirmed **before** any
segment is reclaimed. On a medium that answers `Unsupported`, nothing is ever removed on that
account (`NotConfirmed`). A provider that has accepted no anchor for the stream, or that does not
answer, stops rotation (`NoAnchor`, `ProviderUnavailable`), except that an unavailable provider
may be relied on for the anchor this adapter itself advanced in this start. Before a whole
stream the provider anchors is removed, the ledger's own anchor is advanced past the trim, so a
retirement an interruption leaves owed can be completed at the next start and only then.

`LogVerifier` reads a whole log and states each boundary: `ClosedAt`, `EndedWithoutClose`,
`RemovedUnderRetention`, `LeftoverFromInterruptedRemoval`, `RemovalNotCarriedOut`,
`PrefixMissingWithoutTrim`, `RecordsMissingAfterTrim`, `TrimPastAnchor`,
`OriginClaimedWhileHistoryExists`, the checked citations of `predecessor` and `recovered`, and
others. The 7.5 verdict of each stream and ledger is unchanged: only `Anchored` is "verified", and
always with its range.

## Limits

The ledger lives in the mutable log, so its records are only as trustworthy as its own anchors. A
power loss and a deliberate removal of the last records look the same after an abrupt end. A trim
past a reader's retained checkpoint ends the protection that checkpoint gave its prefix. Nothing
here lets a deployment claim that the log cannot be altered.
