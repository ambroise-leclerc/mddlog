# ADR-004: Audit persistence and tamper evidence

## Status
Proposed — **the least developed of the four records.** It exists to bound what may be claimed while
the design is open, and to hold the open questions in one place. Several decisions below are
deliberately left unanswered and marked as such; do not read an unanswered question as a permissive
default.

## Context

`README.md` promises, under Audit Trail Features, "Tamper-proof records with cryptographic
signatures" and, under Medical Device Compliance, "Regulatory Reporting: Automated compliance report
generation" — both marked `(planned)`. Nothing implements either.

ADR-002 Decision 3 defines a three-level delivery contract — admitted, handed off, durably confirmed
— and states that **no backend offers level 3 today**, leaving that level in the contract purely so a
future record has somewhere to attach. This is that record's slot. mddlog issue #5 likewise excludes
"persistent/cryptographically protected audit storage" from its own scope.

Two constraints from the earlier records shape everything here:

- **Key material and storage cannot live in the governed core.** ADR-001 confines the governed zone
  to non-allocating, non-throwing, non-blocking code with no filesystem or OS surface. Reading a key,
  writing a file, and flushing to durable media are none of those things. Persistence is therefore an
  adapter/host concern by construction, not by preference.
- **The word matters.** "Tamper-proof" claims prevention. Nothing a logging library can do prevents
  someone with write access to the medium from altering or deleting records. What is achievable is
  **tamper-evident**: alteration or truncation becomes detectable after the fact. This record adopts
  the second word and recommends the README be corrected to match before anything here ships.

## Medical Device Considerations

- **Post-market reconstruction** is the purpose: an audit trail earns its cost when someone must
  reconstruct, months later, what a deployed device did. That requires records that survive restart
  and that a reader can check for completeness — not only for content.
- **Detection is not prevention, and both are the manufacturer's problem, not the library's.** A
  hash chain shows that a sequence was altered or truncated; it does not stop it, and it does not
  establish who wrote a record unless a key binds it. Access control over the medium, key custody,
  and device-level physical security sit with the manufacturer's own risk file.
- **A gap must read as a gap.** ADR-002 Decision 5 gives every event a stream identity and a
  per-stream sequence; the persistence format must preserve both, so that a restart, a lost segment
  or a deliberate excision is visible as a discontinuity rather than silently closing up.

## Decision

### 1. Tamper-evident **relative to a trust anchor**, never "tamper-proof"

An earlier revision of this record claimed that *alteration or truncation of a stored audit sequence
is detectable by a reader holding the chain state*, with the chain of Decision 2 as the whole
mechanism. That claim does not survive the threat it names. Against someone who can rewrite the
storage, `H_i = hash(record_i ‖ H_{i-1})` alone detects nothing:

- **rewrite with recomputation** — modify record 2, recompute `H_2`, `H_3`, … and every internal
  link verifies;
- **suffix truncation** — delete the last records and the surviving prefix is still a perfectly
  valid chain.

Neither requires breaking the hash function, and both were confirmed on a three-event chain in
review. A final digest stored beside the log, writable by whoever can write the log, is not an
anchor either — it gets recomputed along with everything else.

So the property is conditional, and the condition is part of the claim:

> Given an **authentic anchor held independently of the mutable log** — at minimum the stream
> identity, the position it covers, and the digest at that position — a reader can detect any
> alteration of, or truncation after, that position. Records after the last trusted anchor are
> covered only by the chain's internal consistency, which a rewriting adversary can reproduce.

What that means in practice:

- the detection promise is **scoped to the last trusted checkpoint**, and a reader must be told how
  far coverage extends rather than being left to assume it reaches the end of the log;
- **a missing or stale anchor is a reportable state, not a pass.** Verification against no anchor
  yields "internally consistent, unanchored" — never "verified". A stale anchor yields verification
  up to its position and nothing beyond;
- signing (Decision 3) is *one* way to supply anchoring, not the only one — a monotonic counter in
  tamper-resistant hardware, a remote witness that receives digests as they are produced, or an
  operator-recorded checkpoint all qualify. What does not qualify is any state a log-rewriting
  adversary can rewrite too. Decision 7 specifies the anchor's contents, the provider interface,
  the exclusions, and what a verifier reports.

**Absent any anchoring mechanism, the claim reduces to internal chain consistency**, and that is
what must be written in user-facing material. README's "tamper-proof" wording should be corrected
independently of whether this record is accepted.

### 2. Hash chaining over a canonical serialization is the minimum mechanism

Each stored record carries a digest over (its canonical bytes ‖ the previous record's digest), so
that altering or removing any record breaks every subsequent link *for a reader who knows where the
chain is supposed to lead* (Decision 1). This is the smallest mechanism that delivers anything
without key management, and it is deliberately the floor rather than the ceiling: anchoring and
signing layer on without changing the record format.

Two prerequisites this record inherits rather than invents:

- a **canonical** serialization — same record, same bytes, on every toolchain. MduX's ADR-007
  pipeline exists for exactly this reason (sorted keys, no locale-dependent formatting, and floats as
  bit patterns rather than decimal text); mddlog cannot import it but should not re-derive the lesson
  from scratch either. **Pointing at that precedent is not a specification, and this record does not
  pretend otherwise**: field order, byte encoding, the rule for absent optional fields, a schema
  version, and the digest's own encoding are all undefined here, and two implementations following
  this text as written would hash different bytes. Fixing them is a blocking prerequisite, listed as
  such in Decision 4 — a canonical format decided late is a format decided twice, because the first
  stored record freezes it;
- the stream identity and per-stream sequence from ADR-002 Decision 5 — note that identity now
  includes the producer instance, not only the boot session, so a chain is scoped to one stream
  instance and a restart begins a new, explicitly linked chain rather than appearing continuous.

### 3. Signing is deferred again, and the reason is key custody

Binding a chain to a device identity requires a private key on the device, which requires answering
where it is stored, how it is provisioned, how it is rotated, and what happens when it is
compromised. None of those are logging questions, and a wrong answer is worse than a missing one.
This record therefore specifies the chain and leaves signing to a later decision, with the record
format required to leave room for a signature over the chain head.

### 4. Open questions, stated rather than defaulted

An implementation must not settle any of these silently. Two of them were **blocking**: nothing
may be stored before they are answered, because the first record written freezes both.

- **The anchoring mechanism** (blocking, Decision 1). **Resolved by Decision 7**: anchor contents,
  the provider interface and eligibility, the exclusions, advancement, independent retention, absence
  and staleness, the reader's monotonic position, and the verifier's verdicts. Decision 7 chooses no
  single provider. A deployment that configures none has internal chain consistency only, and the
  verifier reports exactly that.
- **The canonical byte contract** (blocking, Decision 2) — field order, encoding, absent-optional
  rules, schema version, and digest encoding, specified precisely enough that two independent
  implementations hash identical bytes for identical records.
- **Storage medium and layout** — segmented files, a circular region of flash, or an
  append-only device log. Each has a different truncation and wear story.
- **Power-loss atomicity** — what "durably confirmed" (ADR-002 level 3) means precisely: written,
  flushed, or acknowledged by the medium. Until this is answered, level 3 stays unclaimed.
- **Retention and rotation** — how a bounded medium ages out old records without making the chain
  unverifiable, and what a reader sees at the boundary.
- **Chain state recovery** — where the last digest lives across a restart, and what a reader
  concludes when it is missing or inconsistent.
- **Export format** — the "automated compliance report generation" README claims; likely a separate
  record again, since an export format is read by tools nobody here controls.

### 5. Validation scenarios any implementation must answer

These are acceptance criteria, not illustrations. Each states what a verifier must report; an
implementation that reports "valid" for the first three has not implemented Decision 1.

| Scenario | Expected verifier output |
|---|---|
| **Rewrite with recomputation** — record `k` is altered and every later digest recomputed; the anchor covers position `p` | `k ≤ p`: **Altered** (7.5, verdict 4). The recomputed `H_p` differs from the anchor's digest, even though every internal link checks out, which is why the anchor is required. `k > p`: the alteration cannot be detected. The report says coverage is `1 … p` and that records past `p` are only internally consistent (7.5, limits) |
| **Suffix truncation** — the last *n* records are deleted, leaving last sequence `m`, with the anchor at `p` | `m < p`: **Incomplete** (7.5, verdict 5). Records `m+1 … p` are reported missing, and the prefix is reported internally consistent. `m ≥ p`: only records past the anchor were removed. This cannot be told apart from records never written, and the report says coverage is `1 … p` |
| **Old log restored with its matching old anchor** — both rolled back together | With a retained position (7.4): **Rolled back** (7.5, verdict 2). The anchor's `counter` or `position` is below the retained value, or a retained stream is missing. Without a retained position: **Anchored** up to the old position, with "rollback not excluded: no retained position" stated. The verdict is never an unqualified pass |
| **Missing anchor** | "Internally consistent, unanchored", never "verified" (7.5, verdict 8). An unavailable provider is reported separately as "anchor unavailable". A stale anchor limits coverage to its position (7.3) |
| **Restart** — a new stream instance begins (ADR-002 Decision 5) | A new chain, explicitly linked to the previous stream identity, with the discontinuity visible rather than closed up |

The third scenario is the one that shows why an anchor must be more than a digest: a rollback of log
and anchor together is internally coherent, and only a monotonic position a reader remembers — or a
witness outside the device — distinguishes it from the truth.

### 6. Nothing about this record changes the claims allowed today

Until it is implemented and verified, ADR-002 Decision 3 stands unchanged: mddlog promises in-memory
admission only, and power loss is outside every guarantee. This record's existence is not evidence of
a capability, which is the same caution the index applies to every Proposed record.

### 7. The anchor, its providers, and the monotonic position

This decision answers the first blocking question of Decision 4. It specifies what an anchor states,
what may hold one, how it advances, what a reader retains and what a verifier reports. It chooses
**no single provider**: which provider a deployment uses is the integrator's decision, made against
their own threat model. Nothing here requires a key or a signature (Decision 3). A provider may use
credentials of its own, such as authenticated storage or a channel to a remote service. Those
credentials belong to the provider and the host. They are not part of any mddlog record, and this
record does not specify them.

#### 7.1 Contents of an anchor

An anchor is an **anchor claim**, which states what the log contained, plus a **provider stamp**,
which states where and when the claim was accepted. All of the following fields are mandatory. A
claim missing any of them is not an anchor, and a verifier treats the stream as unanchored (7.5).

| Field | Meaning |
|---|---|
| `anchorFormat` | Version of this anchor layout. A verifier that does not know the version treats the anchor as absent, never as matching. |
| `canonicalVersion` | Version of the canonical byte contract (Decision 2) under which `digest` was computed. A verifier recomputes under that version or reports that it cannot (7.5). It never substitutes another version. |
| `streamId` | The stream instance identity of ADR-002 Decision 5, copied exactly. An anchor covers one stream instance and nothing else. |
| `position` | The `sequence` of the last record covered. Sequences start at 1 (ADR-002 Decision 5), so `position ≥ 1`. The anchor covers records `1 … position` of that stream instance. |
| `digest` | The chain digest `H_position` of Decision 2, in the representation Decision 2 fixes. |
| `providerId` | Identifies the provider instance that accepted the claim, so a reader knows which retained state (7.4) applies. |
| `counter` | Assigned by the provider, not by the log writer. It strictly increases across **every** anchor that provider instance accepts, across all streams. |
| `acceptedTime` | When the provider accepted the claim, by the provider's clock if it has one. Otherwise an explicit "unavailable" value, as in ADR-002 Decision 5. It is used only for age staleness (7.3). |

`streamId`, `position` and `digest` are the minimum of Decision 1. `canonicalVersion` is required
because a digest has no meaning without the bytes it was computed over. The schema-version field of
the canonical contract (issue #84) supplies the value. `counter` lets a rollback that crosses
streams be detected (7.4). A per-stream position alone does not see a stream that has disappeared.

Two anchors **conflict** when they have the same `providerId` and `streamId`, and either the same
`position` with different `digest` values, or positions that decrease while `counter` increases.
A conflict is an integrity finding (7.5), never a tie to resolve by choosing one anchor.

#### 7.2 The anchor provider interface

The provider sits in the adapter zone. It does I/O, so it cannot sit in the governed core
(ADR-001). These operations are an interface contract, not C++ signatures. Issue #90 writes those.

- **`advance(claim)`** offers a claim. The provider answers in one of three ways:
  - **accepted**, with the stamp: `providerId`, `counter` and `acceptedTime`;
  - **refused**, with a reason: `positionNotIncreasing` (the claim's position is not above the last
    accepted position for that stream), `conflict` (the same position with a different digest) or
    `malformed`;
  - **unavailable**: the provider could not be reached or written to. This answer accepts nothing.
- **`latest(streamId)`** returns the highest accepted anchor for that stream, **absent**, or
  **unavailable**.
- **`streams()`** returns every stream identity the provider has accepted an anchor for, each with its
  highest anchor, or **unavailable**. A verifier uses this list to find streams that are missing from
  the log entirely.

A provider is **eligible** only if all of the following hold:

1. **Independent custody.** The writer of the mutable log cannot rewrite or delete what the provider
   has accepted, and cannot return the provider to an earlier state, using the access it uses to
   write the log. A provider that this test would disqualify is not an anchor, whatever its name.
2. **Monotonic acceptance.** The provider enforces the `positionNotIncreasing` and `conflict`
   refusals itself. It does not depend on the caller to behave.
3. **Faithful reads.** `latest` and `streams` return what the provider accepted, or **unavailable**.
   They never return a value fabricated from the log.

The record recognises three kinds of provider. None of them is the default.

- **Tamper-resistant device storage**, such as a hardware monotonic counter paired with
  write-protected or authenticated storage that holds the claim. The counter alone is **not** an
  anchor. A counter can show that a rollback happened (7.4), but it holds no digest. A rewrite with
  recomputation therefore goes undetected unless the claim itself is held in the protected storage.
- **A remote witness**: an off-device service that receives claims as they advance and serves them
  to readers. The witness also acts as the independent monotonic state of 7.4.
- **An operator-recorded checkpoint**: an operator copies a claim into a record kept separately
  from the device, such as a service record. The operator's record assigns the stamp. Checkpoints are
  usually infrequent, so coverage often ends before the end of the log (7.3).

**Excluded explicitly**, because a log-rewriting adversary can rewrite each of them as well:

- a file, partition, region or object on the same medium, or under the same write authority, as the
  log;
- a "last digest" or "chain head" stored next to the log, or inside it;
- process memory or any state the device restores together with the log;
- a value derived from the log itself, such as a final digest recomputed at read time;
- a device-local MAC or signature produced with a key the log writer can use. That option belongs to
  signing (Decision 3), which this record defers.

#### 7.3 Advancement, independent retention, absence and staleness

- **What may be anchored.** An anchor's position never exceeds the highest record the storage
  contract reports as **durably confirmed** (ADR-002 level 3; the storage contract is issue #86).
  Without this rule, power loss after anchoring but before storage would turn into a false
  truncation finding. Until that contract is accepted, no anchor may be advanced, because nothing is
  durably confirmed.
- **Frequency.** The integrator chooses a policy and declares it to the verifier as configuration,
  not as a constant in the library. The policy has two bounds: a record bound `N` (advance once at
  most `N` records lie past the current anchor) and an age bound `T` (advance once the current anchor
  is older than `T` while newer records exist). The policy also requires an attempt **on orderly
  close** of a stream instance, so that its final position is anchored before a restart starts a
  new stream (ADR-002 Decision 5). How a new chain links to the previous stream is issue #87.
- **The exposure window.** Records past the last accepted anchor are covered by internal consistency
  only (Decision 1). `N`, `T` and the outcome of each `advance` together set how large that window
  can grow. This record sets no default for `N` or `T`, because the right values depend on the
  device's event rate and its risk file.
- **Advancement never blocks admission.** `advance` runs in the adapter, after durable confirmation.
  A refusal or an **unavailable** answer is reported through the audit health mechanism (ADR-002
  Decision 3) and retried under the same policy. It is never retried by holding up the ring.
  `positionNotIncreasing` or `conflict` on a fresh claim is not a transient failure. It means that
  the log or the provider has diverged, and it is reported as an integrity fault.
- **Independent retention.** Accepted anchors are retained by the provider, under eligibility
  condition 1, for at least as long as the records they cover are retained. Retention and rotation
  are issue #87. An anchor whose records have aged out may be dropped. A record retained longer than
  its anchors falls back to internal consistency, and the verifier reports that.
- **Absence.** No anchor exists for a stream, either because none was ever accepted or because the
  anchor did not satisfy 7.1. The verifier reports the stream as **unanchored**.
- **Unavailability.** The provider could not answer. The verifier reports **anchor unavailable**,
  which has the same coverage as unanchored but a different cause, so the two are never merged.
- **Staleness by position.** The anchor's position is below the last record present for that stream.
  Coverage is `1 … position`, and the verifier reports the records past it as internally consistent
  and unanchored. Every verdict that applies to an anchored stream states the anchor's position,
  including the full one.
- **Staleness by age.** The anchor's `acceptedTime` is older than the declared bound `T` relative to
  the verification time. If `acceptedTime` is unavailable, the age is reported as unknown. Age
  staleness **does not reduce coverage below the anchor's position**. It warns that the
  advancement policy was not met, and it is reported alongside the coverage.

#### 7.4 The monotonic position held by the reader or a witness

A rollback of the log and its anchor together is internally coherent: the restored anchor matches
the restored log. Only state that the rollback could not reach tells the restored pair apart from the
real one. Decision 5's third scenario depends on that state, which this record calls the **retained
position**:

- **What is retained.** For each `providerId`: the highest `counter` accepted, and for each
  `streamId`, the highest anchored `position` and the `digest` at that position.
- **Who retains it.** Either the **reader**, meaning the verification tooling, kept off the device
  and outside the log writer's authority, or an **independent witness**. A remote-witness provider
  already holds this state. A tamper-resistant counter on the device holds `counter` but not the
  per-stream part, so the reader must keep that part. State the device restores together with its
  log is excluded for the same reason anchors on the log's medium are (7.2).
- **When it is updated.** Only after a verification that finds neither a mismatch nor a conflict, and
  only by raising values, never by lowering them. A failed verification leaves the retained position
  unchanged, so a rollback that has been reported cannot be accepted silently afterwards.
- **What it detects.** An anchor, or a `streams()` result, is **rolled back** when any of the
  following holds:
  - its `counter` is below the retained counter for that provider;
  - its `position` is below the retained position for that stream;
  - it has the same position as the retained position but a different digest;
  - a stream the retained position records is missing from both the provider and the log.
- **The first verification.** With no retained position, a joint rollback cannot be excluded. The
  verifier says so: "rollback not excluded: no retained position". A rollback finding is never
  implied by silence.

#### 7.5 What a verifier reports

The verifier reports each stream instance separately and never folds them into one "valid" result.
A report states the following:

- the coverage, as a sequence range: `1 … p` anchored, and `p+1 … m` internally consistent, where
  `m` is the last record present;
- the anchor's `providerId`, `counter`, `canonicalVersion` and `acceptedTime`;
- the outcome of the retained-position check;
- one **verdict** from the list below, the first that applies.

1. **Inconsistent**: a chain link fails between two records present. The report names the first
   failing sequence.
2. **Rolled back**: the retained-position check of 7.4 fails.
3. **Conflict**: two anchors for the stream conflict (7.1).
4. **Altered**: the recomputed `H_p` differs from the anchor's `digest`. Some record at or before `p`
   was changed, or a record was inserted or removed at or before `p`.
5. **Incomplete**: the last record present has a sequence `m < p`. Records `m+1 … p` are missing. The
   prefix is internally consistent but cannot be matched against `digest`, because `H_p` cannot be
   computed.
6. **Cannot verify**: the anchor's `canonicalVersion` or `anchorFormat` is unknown to the verifier.
7. **Anchored**: `H_p` matches. The report states `p`, and lists records past `p` as internally
   consistent and unanchored. When no retained position existed, it adds "rollback not excluded".
8. **Unanchored** or **anchor unavailable**: "internally consistent, unanchored", together with the
   cause.

No verdict is called "verified" unless it is verdict 7, and verdict 7 always carries its position.
The word never appears without the range it applies to.

**Limits this record states rather than hides.** An alteration or truncation that affects only
records **after** the last anchor cannot be detected. A rewriting adversary can reproduce internal
consistency there, and a truncated suffix cannot be told apart from records never written. The
exposure window of 7.3 bounds this limit. Nothing removes it. A verifier with no retained position
cannot exclude a joint rollback of log and anchor. An eligible provider that is itself compromised
defeats the mechanism. Choosing a provider fit for the device's threat model is part of the
manufacturer's risk file, not the library's.

## Alternatives Considered

### 1. Signed records from the start, no separate chaining step (Rejected for now)
**Pros:** One mechanism, stronger claim, no migration from chain-only to chain-plus-signature.
**Cons:** Blocks all persistence on key custody (Decision 3). Chaining delivers the detectable-
alteration property immediately and leaves the format open for signatures.

### 2. Rely on the storage medium's own integrity (Rejected)
**Pros:** No work in the library; filesystems and flash controllers already detect corruption.
**Cons:** They detect *accidental* corruption. They do not detect a deliberate, well-formed
rewrite, which is the case an audit trail exists for.

### 3. Persist every log record, not only audit events (Rejected)
**Pros:** One path; no decision about what deserves durability.
**Cons:** Volume. Diagnostics at debug level on a busy device would dominate the medium and force
retention policies that then age out the audit records too. ADR-002's separation of audit from
severity exists precisely so this choice can be made once, correctly.

## Consequences

### Positive
- Gives ADR-002's third delivery level a named home instead of an open-ended promise.
- Replaces "tamper-proof" with a claim that can actually be verified, before any code exists to
  overclaim about.
- Keeps key custody out of a logging decision.

### Negative
- Chaining adds a per-record digest computation on the persistence path and a verification step for
  any reader; neither is free.
- The detection property depends on an anchor provider that Decision 7 specifies but does not
  choose. A deployment gets only as much assurance as the provider it configures, and the exposure
  window past the last anchor stays undetectable by construction.
- Detecting a joint rollback of log and anchor requires state kept off the device (Decision 7.4).
  The reader's tooling has to keep and protect that state, which is an operational burden outside
  the library.
- Six questions remain open (Decision 4), and the canonical byte contract is still blocking. This
  record therefore cannot be implemented as it stands. It bounds the design rather than settling it.

### Risks and Mitigations
- **The record is read as a plan rather than a boundary.** *Mitigation*: Status and Decision 6 both
  say what it is; the index marks it Proposed like the rest.
- **The chain is mistaken for the whole mechanism.** This is not hypothetical — the previous
  revision of this record made exactly that error, claiming detection from chaining alone when a
  rewriting adversary defeats it by recomputation or truncation. *Mitigation*: Decision 1 states the
  conditional property and its scope, and Decision 5's first three scenarios fail any implementation
  that reproduces the error.
- **A chain is treated as proof of authorship.** *Mitigation*: Decision 1 states what the claim is
  and is not, in the words to use.
- **Canonical serialization is re-derived badly, or decided late.** *Mitigation*: Decision 2 names
  the prerequisite, says plainly that pointing at MduX ADR-007 is not a specification, and Decision 4
  marks the byte contract blocking — the first stored record freezes it.

## References
- ADR-002 Decision 3 and Decision 5 (this repository) — the delivery level this record would fill,
  and the stream identity/sequence a chain is scoped by.
- ADR-001 (this repository) — why key material and storage cannot be governed-zone concerns.
- [MduX ADR-007: Evidence pipeline doctrine](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/ADR-007-evidence-pipeline-doctrine.md) — canonical, byte-stable serialization and the determinism failure modes Decision 2 inherits.
- mddlog issue #5 — the explicit exclusion of persistent/cryptographically protected audit storage.
- mddlog issues #84 (canonical byte contract and its schema version, which `canonicalVersion` cites),
  #86 (storage and the meaning of durably confirmed, which bounds an anchor's position) and #87
  (restart, rotation and retention, which bound how long anchors are kept). Decision 7 depends on
  all three. #90 implements the provider interface and verifier specified there.

## Approval
- **Decision Date**: not yet approved — drafted for review.
- **Approved By**: pending (project maintainer).
- **Review Date**: when Decision 4's open questions are answered, which is a precondition for any implementation work.
