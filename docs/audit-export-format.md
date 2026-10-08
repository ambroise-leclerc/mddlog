# Audit export formats v1 — candidate contract

The two formats below are independent of canonical/layout/anchor and C++ API versions.
[ADR-006](adr/ADR-006-audit-tools-and-export.md) records the design proposed for review.
Do not interpret this candidate as #121's final stability commitment.

## Projection JSON

Root `schema="mddlog.audit.projection"`, `version=1`, `libraryVersion="0.3.0"`.
Required root fields are `textEncoding`, `source`, `providerProvenance`, `trust`,
`retainedTrust`, `selection`, `verificationTimeNs`, `maxAnchorAgeNs`, `exitCode`,
`report` and `events`. Integers are exact JSON integers; consumers must not round
64-bit positions/counters/times through IEEE-754 doubles. Unavailable optional values
are JSON null. Arrays follow library report/image order. Text uses `byte-latin1`:
each original byte maps to the corresponding U+0000..U+00FF code point, escaped outside
printable ASCII. Encode parsed strings as Latin-1 to recover original bytes; decode those
bytes as UTF-8 only when the producer's contract says UTF-8. Digests and canonical bytes
are lowercase hex strings. This avoids replacing malformed UTF-8 or normalizing data.

`selection`: `complete` boolean and selected `streamId` or null. Complete means unfiltered
projection of all decodable image records, **not** complete anchored history. An unknown
layout or damaged frame may prevent event decoding; `report` and `exitCode` still govern.
A projection cannot be imported for evidence verification, even though individual event
bytes are included: segment headers, unknown regions and provider snapshots are missing.

`report`: medium unreadability, resource issue ID, unknown layout version or null, count
of headerless segments, provider calls, resource usage (`segments`, `totalBytes`,
`bytesRead`, `readCalls`, `records`), `streams`, `unlisted`, `boundaries`.
Each stream has `ledger`, disposition ID, residual `trailing` (segment index/offset/length
or null) and a nested stream `report`. That report includes stream identity, verdict text
and ID, cause/chain finding IDs, failed position, unknown layout version,
`firstRetained`, `lastPresent`, `anchoredThrough`, `unanchoredFrom`, anchor or null,
age ID, retained outcome ID and `rollbackNotExcluded`. Unlisted reports have the same
stream-report fields. Anchor fields: format, canonical version, stream identity,
position, hex digest, provider identity, counter and accepted Unix epoch nanoseconds
or null. Boundary fields: kind text/ID, stream, other identity, position, second position,
ledger identity and ledger position; unused fields retain the library's empty/zero values.

Each event includes stream identity from the enclosing segment, canonical version,
decode status ID, `canonicalHex`, stored `digest` and decoded `fields` or null.
Decoded fields include sequence, category/phase IDs (explicit v1 mappings below),
Unix epoch `timeNs` or null, action/actor/target, requirement/risk/correlation references,
source sequence or null, detail and detailTruncated. Bytes are projected even when the
chain report is adverse; decoded fields are not an assertion of authenticity.

Consumers reject unknown root schema/version, missing fields and unknown IDs if their
usage requires interpretation. Future additive fields require an explicit compatibility
decision; no silent major-format reinterpretation is permitted. IDs below are mapped
explicitly by the serializer, independently of C++ enum ordering; spelling/text is
informational and IDs are normative for v1. No JSON parser or projection-to-journal importer
is provided: replay accepts evidence only.

## Evidence binary

All numbers are unsigned 64-bit little-endian unless specified. `blob` is byte length
followed by exactly that many bytes. `text` is a blob of original bytes. No platform
structs, alignment, compression, path extraction or serialized C++ objects occur.

Fields in order:

1. Eight magic bytes `MDDAUDIT`, then version number 1.
2. Source text; provider-provenance text (each within provider text budget).
3. Verification time blob: two numbers, availability (0/1) and two's-complement signed
   Unix epoch nanoseconds. Unavailable time requires both numbers zero.
4. Max age number: 0 means undeclared; otherwise max age in nanoseconds plus 1. Values
   greater than INT64_MAX are rejected.
5. Twelve resource numbers: maxSegments, maxSegmentBytes, maxTotalBytes, readChunkBytes,
   maxReadBytes, maxStreams, maxRecords, maxProviderEntries, maxProviderTextBytes,
   maxProviderCalls, maxIntegrityFaults, reserved zero. All limits must be positive,
   fit the host representation, and not exceed local admission budgets.
6. Provider-listing blob (maximum 1 MiB); empty means unavailable. Nonempty encoding uses
   [WitnessCodec](../include/mddlog/adapter/WitnessCodec.cppm)'s v1 listing contract,
   pinned here: provider identity text, head number, entry count; each entry has retirement
   tag 0/1 and an anchor (format number, canonical-version number, stream text, position,
   32 digest bytes, provider text, counter, two-number time). Tag 1 adds retirement counter
   and two-number retirement time. Entries must have unique streams/counters, mandatory
   fields, matching provider identity and counters consistent with the head. Wire changes
   to WitnessCodec require an evidence version decision. Unknown anchor/canonical versions
   are retained as inputs and receive the library's CannotVerify result.
7. Retained head count, then provider identity text and head for each entry.
8. Retained anchor count, then stream text, position, counter, retired 0/1 and 32 digest
   bytes for each entry. Identities are unique; positions/counters positive.
9. Segment count, then nonzero unique opaque reference number and blob of **every original
   segment byte** for each entry. Segment and total budgets apply before copying. Headers,
   frames, stored digests, ledgers, unknown or residual bytes are never re-encoded.
10. 32-byte SHA-256 of all preceding bytes. This is a checksum, never a signature.

No appended bytes are permitted. Truncation, checksum mismatch, invalid tags/identities,
unsupported version, excessive input size or profile enlargement are operation errors.
The read-only EvidenceMedium feeds original bytes into LogVerifier/LogImage. Reports are
**recomputed**, using the retained/time/profile/provider inputs, rather than serialized as
an authoritative cached verdict. Thus event/report export is JSON and the evidence package
is the raw reproduction input; a companion projection is optional. A frozen listing makes
both exporter and replay use the same latest/streams answers. Authenticity still requires
an independent source or explicitly accepted embedded-snapshot assumption.

Default archive/output cap: 150994944 bytes. Defaults for all resource fields are listed
in [CLI usage](audit-tools.md). Before accepting a larger package configure local limits
explicitly. Decoder work/memory is bounded by these limits and the hard witness metadata
limits; they are not an RSS or execution-time guarantee.

## Stable IDs

### Verdict

| ID | Meaning |
|---|---|
| 0 | `Inconsistent` |
| 1 | `CannotVerify` |
| 2 | `RolledBack` |
| 3 | `Conflict` |
| 4 | `Altered` |
| 5 | `Retired` |
| 6 | `Incomplete` |
| 7 | `Anchored` |
| 8 | `Unanchored` |
| 9 | `AnchorUnavailable` |

### VerdictCause

| ID | Meaning |
|---|---|
| 0 | `None` |
| 1 | `InvalidStreamIdentity` |
| 2 | `UnsupportedStreamVersion` |
| 3 | `UnsupportedLayoutVersion` |
| 4 | `StorageUnreadable` |
| 5 | `SegmentLayoutInconsistent` |
| 6 | `AnchorFormatUnknown` |
| 7 | `AnchorMissingField` |
| 8 | `TrimPastAnchor` |
| 9 | `ProviderHeadBelowRetained` |
| 10 | `StreamAnchorBelowRetained` |
| 11 | `RetirementUndone` |
| 12 | `StreamMissing` |
| 13 | `AnchorConflict` |
| 14 | `AnchorDigestMismatch` |
| 15 | `RetainedCheckpointMismatch` |
| 16 | `LogEndsBeforeAnchor` |
| 17 | `LogEndsBeforeRetained` |
| 18 | `NoAnchor` |
| 19 | `ProviderUnavailable` |
| 20 | `LedgerRecordMalformed` |
| 21 | `TrimDigestMismatch` |
| 22 | `LeftoverDoesNotReachTrim` |
| 23 | `TrimNotOnSegmentBoundary` |
| 24 | `PrefixMissingWithoutTrim` |
| 25 | `RecordsMissingAfterTrim` |
| 26 | `TrimExceedsRecords` |
| 27 | `TrimDiffersFromRetirement` |
| 28 | `RetirementBeyondTrim` |
| 29 | `ResourceLimit` |

### ChainFinding

| ID | Meaning |
|---|---|
| 0 | `Ok` |
| 1 | `Malformed` |
| 2 | `InvalidVerifier` |
| 3 | `VersionChange` |
| 4 | `UnsupportedVersion` |
| 5 | `StreamMismatch` |
| 6 | `SequenceBreak` |
| 7 | `DigestMismatch` |

### AgeStatus

| ID | Meaning |
|---|---|
| 0 | `NotChecked` |
| 1 | `Fresh` |
| 2 | `Stale` |
| 3 | `Unknown` |

### RetainedOutcome

| ID | Meaning |
|---|---|
| 0 | `NoneRetained` |
| 1 | `Passed` |
| 2 | `RolledBack` |
| 3 | `NotChecked` |
| 4 | `CheckpointTrimmed` |

### StreamDisposition

| ID | Meaning |
|---|---|
| 0 | `Whole` |
| 1 | `Rotated` |
| 2 | `RemovalNotCarriedOut` |
| 3 | `InterruptedRemoval` |
| 4 | `GapAfterTrim` |
| 5 | `PrefixMissing` |
| 6 | `TrimNotOnSegmentBoundary` |
| 7 | `Removed` |
| 8 | `Absent` |

### BoundaryKind

| ID | Meaning |
|---|---|
| 0 | `PredecessorMatched` |
| 1 | `PredecessorNotReproduced` |
| 2 | `PredecessorNotCheckable` |
| 3 | `PredecessorFailedForm` |
| 4 | `RecoveredMatched` |
| 5 | `RecoveredNotReproduced` |
| 6 | `RecoveredNotCheckable` |
| 7 | `RecoveredFailedForm` |
| 8 | `ClosedAt` |
| 9 | `ClosedCitationNotReproduced` |
| 10 | `EndedWithoutClose` |
| 11 | `NoCloseRecorded` |
| 12 | `LedgerNotClosed` |
| 13 | `RemovedUnderRetention` |
| 14 | `RemovedNeverAnchored` |
| 15 | `RemovedWithoutAnchor` |
| 16 | `LeftoverFromInterruptedRemoval` |
| 17 | `RemovalNotCarriedOut` |
| 18 | `PrefixMissingWithoutTrim` |
| 19 | `RecordsMissingAfterTrim` |
| 20 | `TrimPastAnchor` |
| 21 | `OriginClaimedWhileHistoryExists` |
| 22 | `NoEarlierHistoryKnown` |
| 23 | `LedgersFork` |
| 24 | `LedgerCitationCycle` |
| 25 | `StreamNotOpenedByAnyLedger` |
| 26 | `ReservedActionOutsideLedger` |
| 27 | `LedgerRecordMalformed` |

### AuditResourceIssue

| ID | Meaning |
|---|---|
| 0 | `None` |
| 1 | `InvalidLimits` |
| 2 | `Segments` |
| 3 | `SegmentBytes` |
| 4 | `TotalBytes` |
| 5 | `ReadBytes` |
| 6 | `Streams` |
| 7 | `Records` |
| 8 | `ProviderEntries` |
| 9 | `ProviderCalls` |
| 10 | `ProviderTextBytes` |
| 11 | `IntegrityFaults` |
| 12 | `MemoryUnavailable` |

### CanonicalReadStatus

| ID | Meaning |
|---|---|
| 0 | `Ok` |
| 1 | `Malformed` |
| 2 | `UnsupportedVersion` |


### AuditCategory

| ID | Meaning |
|---|---|
| 0 | Lifecycle |
| 1 | Configuration |
| 2 | Access |
| 3 | RiskControl |
| 4 | Operator |

### AuditPhase

| ID | Meaning |
|---|---|
| 0 | Requested |
| 1 | Confirmed |
| 2 | Executed |
| 3 | Failed |
