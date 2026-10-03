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
stream identities, and their sequences begin at 1 again. An identity that a ledger in the log
names, even one whose records were removed, is refused (`streamIdentityInUse`), and so is a
new ledger identity a ledger in the log names (`LedgerIdentityInUse`). A reader reports the old and the new
instances separately, so a discontinuity is never presented as continuity.

Inconsistent state is written in its `Failed` form, citing the last position that checks, and
reported through `health().integrity`. A ledger with a malformed record checks only up to the
record before it. Ledgers on a cycle of `predecessor` citations are inconsistent too, even when another
ledger cites into the cycle. When every ledger is cited, record 1 cites one of them in `Failed`
form: `origin` is written only when the log holds no ledger. Records are never repaired, rewritten or reordered, and
retention never removes records of a stream recovery found inconsistent. A log that cannot be
read in full, because a segment is unreadable or in a layout version this reader does not know,
leaves every stream unverifiable (`Unverifiable`): nothing is removed and no owed retirement is
relayed, since that segment may hold any stream's last records.

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
segment is reclaimed. On a medium that answers `Unsupported`, no trim is written and nothing is
removed (`NotConfirmed`). A provider that has accepted no anchor for the stream, or that does not
answer, stops rotation (`NoAnchor`, `ProviderUnavailable`), except that an unavailable provider
may be relied on for the anchor this adapter itself advanced in this start. Before a whole
stream the provider anchors is removed, the ledger's own anchor is advanced past the trim, so a
retirement an interruption leaves owed can be completed at the next start and only then. That
retirement is relayed only when the trim reaches every position a `close` or `recovered` record
cites for the stream: a rotation's trim followed by the loss of the later records is reported
(`RecordsMissing`), never retired. An earlier ledger is kept, beyond the conditions of 10.5, while
it holds the highest trim of a stream the log still holds or of a removed stream whose retirement
is still owed: removing it would erase the only record of that removal (`LedgerStillNeeded`).
`relieve()` reads the log and recovers chain state once, and again only after an attempt that
wrote or removed something.

`LogVerifier` reads a whole log and states each boundary: `ClosedAt`, `EndedWithoutClose`,
`RemovedUnderRetention`, `LeftoverFromInterruptedRemoval`, `RemovalNotCarriedOut`,
`PrefixMissingWithoutTrim`, `RecordsMissingAfterTrim`, `TrimPastAnchor`,
`OriginClaimedWhileHistoryExists`, `LedgerCitationCycle`, the checked citations of `predecessor` and `recovered`, and
others. The 7.5 verdict of each stream and ledger is unchanged: only `Anchored` is "verified", and
always with its range. A removed stream is Incomplete rather than Retired when the provider retired
it past its highest trim (`RetirementBeyondTrim`) or a ledger cites a position past that trim
(`RecordsMissingAfterTrim`). Records missing at the start do not stop the other checks of 7.5: a
rollback, a conflict or an alteration is still reported first, and the missing prefix is stated as
a boundary.

## Limits

The ledger lives in the mutable log, so its records are only as trustworthy as its own anchors. A
power loss and a deliberate removal of the last records look the same after an abrupt end. A trim
past a reader's retained checkpoint ends the protection that checkpoint gave its prefix. Nothing
here lets a deployment claim that the log cannot be altered.
