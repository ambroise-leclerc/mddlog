# ADR-004 validation: coverage demonstrated by the persistence lots (#93)

This report states which behaviour of the audit persistence adapter the tests demonstrate, against
which criteria, and where the demonstration stops. It closes milestone C of epic #11. It is
engineering evidence for review, not a certification dossier, and it does not claim validation in a
production device. Reviewing this evidence, and deciding whether the implementation of
[ADR-004](adr/ADR-004-audit-persistence-and-tamper-evidence.md) is accepted, is a separate decision
of the maintainer; nothing here makes that decision.

## What is implemented

The persistence lots #89 to #92 add, in the adapter zone only:

- the canonical byte contract and per-stream hash chaining (Decision 8, #89);
- the anchor provider interface, the reader's retained position, and the verifier with the verdicts
  of 7.5 (Decision 7, #90);
- the persisting sink over a storage abstraction, durable confirmation and storage health
  (Decision 9, #91);
- the ledger, restart and chain state recovery, rotation and retention, and the log reader's report
  at each boundary (Decision 10, #92).

The governed core gains one rule only: producer admission refuses the reserved `mddlog.` action
prefix. It performs no input/output and holds no key.

## How each milestone C criterion is demonstrated

Each criterion of milestone C (epic #11) is covered by at least one SpecLab scenario. The
`audit-e2e-*` scenarios of [AuditEndToEndSpec.cpp](../tests/spec/AuditEndToEndSpec.cpp) run the
whole path: a producer's `AuditRing`, the `AuditSinkAdapter` consumer, the `PersistingAuditSink`
with its ledger, the anchor provider, and the `LogVerifier` reading the medium. The other scenarios
exercise one component in more cases.

| Milestone C criterion | End-to-end scenario | Component scenarios |
| --- | --- | --- |
| Reference canonical vectors, bytes and digests compared on several toolchains | — | `audit-canonical-vectors` ([AuditCanonicalSpec.cpp](../tests/spec/AuditCanonicalSpec.cpp)), run by every CI configuration below |
| A rewritten event with recomputed digests fails against the authentic anchor | `audit-e2e-tampering` | `audit-anchor-verifier` |
| A deleted suffix the anchor covers is reported Incomplete | `audit-e2e-tampering` | `audit-anchor-verifier` |
| An old log restored with its old anchor is reported rolled back, through the independent monotonic state | `audit-e2e-tampering` | `audit-anchor-retained-position` |
| Missing anchor: "internally consistent, unanchored", never "verified"; stale anchor: coverage limited explicitly | `audit-e2e-anchor-states` | `audit-anchor-verifier` |
| Restart, rotation, retention, absent and inconsistent chain state, cuts at write and acknowledgement points | `audit-e2e-boundaries`, `audit-e2e-resumption`, `audit-e2e-durability` | `audit-restart-*`, `audit-rotation*`, `audit-retention-*`, `audit-storage-power-cuts`, `audit-restart-cuts`, `audit-retention-cuts`, `audit-ledger-*` |
| No durable acknowledgement before the durability contract holds; a failing storage is observable | `audit-e2e-durability` | `audit-storage-durable`, `audit-storage-failures`, `audit-storage-full` |
| Documentation states exactly the coverage and limits demonstrated | — | this report |

## How each scenario of Decision 5 is answered

| Decision 5 scenario | What the end-to-end test checks |
| --- | --- |
| Rewrite with recomputation | Record 4 rewritten with every later digest recomputed: every link checks, and the reader reports **Altered** (`AnchorDigestMismatch`) against the anchor at 10. Record 12 rewritten: **Anchored** through 10 only, 11 … 15 internally consistent and unanchored. |
| Suffix truncation | Records kept through 7 with the anchor at 10: **Incomplete** (`LogEndsBeforeAnchor`), last present 7. Kept through 12: **Anchored** through 10, and nothing distinguishes the removed 13 … 15 from records never written. |
| Old log restored with its old anchor | With the position the reader retained at 15: **Rolled back**. Without a retained position: **Anchored** through 10 with "rollback not excluded". |
| Missing anchor | No provider: **Unanchored** (`NoAnchor`). Provider unavailable: **anchor unavailable**. Stale anchor: **Anchored** through its position only, stale by position and by age against the declared bound. |
| Resumption after storage alteration | Record 3, at or before the anchor, altered while stopped: the restart reports `ContinuityFailed` through audit health, cites the old stream in `Failed` form, advances nothing for it, and the reader reports it **Altered**; the new instance starts at 1. Record 12, past the anchor: no fault, and the reader reports **Anchored** through 10, the reloaded records unanchored. |
| Power loss while writing | A cut before, during or after an append: every durably confirmed record is read back, a partial frame is reported as trailing bytes and read as no record, and the next start succeeds. |
| Restart | The new ledger cites the old one ("predecessor matched"), the old stream is "closed at 5", and the new instance has its own chain from sequence 1. |
| Rotation | After a trim of whole segments, the stream is **Anchored** from `q + 1` through the anchor, and the removal is stated with its trim. |

Two further end-to-end checks cover the durability criterion: on a medium that answers
`Unsupported`, nothing is durably confirmed, no anchor claim exists and the reader reports
**Unanchored**; and a failed `sync` ends the instance in the sink's health (`SyncFailed`, durable
through the last confirmed record), is reported as a loss after admission by the consumer, and leaves
later events in the producer's ring.

## Toolchains and how results are reported

CI builds and runs the whole suite on each configuration of the README: MSVC 19.40+ on Windows x64,
GCC 16.1 / libstdc++ and upstream Clang 21 / libc++ on Linux x86_64, and upstream Clang 21.1.8 /
libc++ on macOS arm64, with ASan + UBSan legs for GCC 16.1 and Clang 21, clang-tidy 21 and
clang-format 21. Each job configures, builds (compilation and linking), then runs CTest as separate
steps, so a configuration that succeeds is never reported as a build, and a build is never reported
as a test result. The TSan leg covers `RingLog` only; the persisting sink is single-consumer by
contract and has no concurrency test of its own.

To reproduce locally with a preset:

```bash
cmake --preset ninja-clang
cmake --build --preset ninja-clang
ctest --preset ninja-clang --output-on-failure
```

## Limits the tests demonstrate, and limits they do not lift

The verdicts above are what ADR-004 Decision 1 allows: integrity **relative to an authentic anchor
held outside the mutable log**. The tests demonstrate that the implementation reports these limits;
they do not remove any of them.

- **Not tamper-proof.** Anyone who can write the medium can alter, truncate or remove records. What
  the reader detects is a disagreement with an anchor or with a retained position.
- **No proof of authorship.** Chaining shows only that records are consistent with each other and
  with an anchor. Nothing is signed (Decision 3, deferred by Decision 11), so nothing shows who
  wrote a record.
- **Past the last anchor, nothing is detectable.** An alteration or a truncation that affects only
  records after the last anchor cannot be detected, and a truncated suffix cannot be told from
  records never written. The exposure window between anchors bounds this.
- **A rollback of log and anchor together** is detected only with a position the reader retained
  earlier. Without one, the report says "rollback not excluded".
- **A compromised provider, or a compromise of the authority to advance or retire,** defeats the
  mechanism. The verifier reports what the anchors attest.
- **The ledger lives in the mutable log.** Its records are only as trustworthy as its own anchors. A
  power loss and a deliberate removal of the last records look the same after an abrupt end.

## What these tests do not demonstrate

- **No real storage medium.** Every scenario runs on `InMemoryStorageMedium`, a test double that
  models the storage contract of Decision 9.2 and injects failures and power cuts at each call. The
  library ships no file, flash or device-log backend. Whether a real medium meets 9.2, and so whether
  "durably confirmed" (ADR-002 level 3) holds on it, is for the integrator to establish on that
  medium.
- **No real anchor provider.** Every scenario uses `InMemoryAnchorProvider`. Choosing and qualifying
  a provider that is independent of the log, as Decision 7 requires, is part of the manufacturer's
  risk file.
- **No persisted retained position.** `RetainedPosition` holds values; keeping them off the device
  and outside the log writer's authority is the reader's responsibility.
- **No field evidence.** The tests establish the behaviour of the exercised implementation on the
  CI configurations. They are not a validation of any device, deployment or production use, and
  mddlog is not certified.

Signing, key custody and an export format remain deferred by ADR-004 Decision 11.
