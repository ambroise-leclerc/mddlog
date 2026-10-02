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
  from scratch either. **Pointing at that precedent is not a specification.** Field order, byte
  encoding, the rule for absent optional fields, a contract version and the digest's encoding were a
  blocking prerequisite (Decision 4): a canonical format decided late is a format decided twice,
  because the first stored record freezes it. Decision 8 specifies them, with reference vectors;
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
  the provider interface and eligibility, the exclusions, advancement and resumption, independent
  retention and retirement, absence and staleness, the reader's monotonic position, the verifier's
  verdicts, and the threat model they assume. Decision 7 chooses no single provider. A deployment
  that configures none has internal chain consistency only, and the verifier reports exactly that.
- **The canonical byte contract** (blocking, Decision 2). **Resolved by Decision 8**: the contract
  version, field order and encoding, the absent, empty and zero rules, the reading rules, SHA-256
  and the chain input with its initial value, the digest's representation, evolution, the room left
  for a signature, and reference vectors that fix every case byte for byte.
- **Storage medium and layout**. **Resolved by Decision 9**: append-only segments, each holding one
  stream instance and removed only whole, on files, a circular flash region turned one segment at a
  time, or an append-only device log. Decision 9 also fixes the storage abstraction, its
  eligibility conditions, and the framing of the stored bytes.
- **Power-loss atomicity**. **Resolved by Decision 9**: "durably confirmed" (ADR-002 level 3) means
  acknowledged by the medium, at the return of a `sync` that answered durable, as a prefix of the
  stream instance. Every power-cut point has a defined outcome, and a partial record is recognised
  by its frame. Level 3 stays unclaimed until an implementation is verified (Decision 6).
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
| **Rewrite with recomputation** — record `k` is altered and every later digest recomputed; the anchor covers position `p` | `k ≤ p`: **Altered** (7.5). The recomputed `H_p` differs from the anchor's digest, even though every internal link checks out, which is why the anchor is required. `k > p`: the alteration cannot be detected. The report says coverage is `1 … p` and that records past `p` are only internally consistent (7.5, limits) |
| **Suffix truncation** — the last *n* records are deleted, leaving last sequence `m`, with the anchor at `p` | `m < p`: **Incomplete** (7.5). Records `m+1 … p` are reported missing, and the prefix is reported internally consistent. `m ≥ p`: only records past the anchor were removed. This cannot be told apart from records never written, and the report says coverage is `1 … p` |
| **Old log restored with its matching old anchor** — both rolled back together | With a retained position (7.4): **Rolled back** (7.5). The provider head is below the retained head, the stream's anchor is below its retained anchor, or a retained stream is missing without a retirement. Without a retained position: **Anchored** up to the old position, with "rollback not excluded: no retained position" stated. The verdict is never an unqualified pass |
| **Missing anchor** | "Internally consistent, unanchored", never "verified" (7.5, **Unanchored**). An unavailable provider is reported separately as "anchor unavailable". A stale anchor limits coverage to its position (7.3) |
| **Resumption after storage alteration** — the storage is altered while the adapter is stopped, and the adapter then restarts | Alteration at or before the last anchor `p`: the adapter's continuity check (7.3) fails. It advances nothing for the old stream, reports an integrity fault through audit health, and starts the new stream instance with the discontinuity visible. The verifier reports the old stream as **Altered** (7.5). Alteration only past `p`: the check passes, the adapter never anchors those reloaded records (7.3), and the verifier reports them as internally consistent and unanchored. The alteration is not detectable, which is the exposure-window limit of 7.5 |
| **Power loss while writing** — the cut falls before, during or after an append, or during a `sync` | The reader reports the trailing bytes of the stream's last segment (9.4) and reads no record from them. A complete frame past the durable position is a record, internally consistent and unanchored. No record at or below the durable position is missing, so no anchor reports a loss (9.5) |
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

**Threat model.** The guarantee covers an adversary who can rewrite the log's storage but cannot
authorize anchors to advance or retire. That holds whether the adversary would act directly or by
diverting the adapter. The adapter, together with its resumption procedure (7.3), lies inside the
trust boundary. The provider described here accepts claims without checking that the chain is
continuous: it cannot, because it never sees the records. Whoever holds the authority to advance or
retire can therefore substitute a rewritten history for any guarantee that rests on the latest anchor
alone. A compromise of that authority is **outside the covered model**. Only a retained checkpoint
the reader verified earlier (7.4) still protects the prefix up to that checkpoint. A provider that
keeps and exposes its whole history would offer a stronger guarantee. That option widens the storage,
read and retention contract, and it is left to a separate decision.

#### 7.1 Contents of an anchor

An anchor is an **anchor claim**, which states what the log contained, plus a **provider stamp**,
which states where and when the claim was accepted. All of the following fields are mandatory.

An anchor the provider returns is either **usable** or **unusable**. It is unusable when its
`anchorFormat` or `canonicalVersion` is unknown to the verifier, or when a mandatory field is
missing. An unusable anchor yields **Cannot verify** (7.5), every time. It is never treated as
absent, because the provider does hold an anchor, and it is never treated as matching. Only a
stream for which the provider holds no anchor at all is **unanchored**.

| Field | Meaning |
|---|---|
| `anchorFormat` | Version of this anchor layout. If the verifier does not know it, the anchor is unusable. |
| `canonicalVersion` | Version of the canonical byte contract (Decision 2) under which `digest` was computed. A verifier recomputes under that version. If it does not know the version, the anchor is unusable. A verifier never substitutes another version. |
| `streamId` | The stream instance identity of ADR-002 Decision 5, copied exactly. An anchor covers one stream instance and nothing else. |
| `position` | The `sequence` of the last record covered. Sequences start at 1 (ADR-002 Decision 5), so `position ≥ 1`. The anchor covers records `1 … position` of that stream instance. |
| `digest` | The chain digest `H_position` of Decision 2, in the representation 8.4 fixes. |
| `providerId` | Identifies the provider instance that accepted the claim, so a reader knows which retained state (7.4) applies. |
| `counter` | Assigned by the provider, not by the log writer. Each accepted anchor and each retirement (7.2) takes the provider's next counter value, so counters strictly increase across all streams. A stream's anchor therefore usually has a counter well below the provider's current **head** (7.2). That is normal, not a rollback (7.4). |
| `acceptedTime` | When the provider accepted the claim, by the provider's clock if it has one. Otherwise an explicit "unavailable" value, as in ADR-002 Decision 5. It is used only for age staleness (7.3). |

`streamId`, `position` and `digest` are the minimum of Decision 1. `canonicalVersion` is required
because a digest has no meaning without the bytes it was computed over. The contract version of
Decision 8 supplies the value; it equals the version carried by the stream's records (8.1). `counter` lets a rollback that crosses
streams be detected through the provider's head (7.4). A per-stream position alone does not see a
stream that has disappeared.

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
- **`retire(streamId, position)`** records that a stream's records have aged out under the retention
  policy (issue #87). `position` is the stream's highest accepted position. The provider answers
  **accepted** with a counter, **refused** (`conflict` if `position` is not the highest accepted
  position, or `unknownStream`), or **unavailable**. A **retirement** keeps the stream's
  `streamId`, its final `position` and `digest`, the counter of that final anchor, and the
  retirement's own counter and time. The provider retains it under the same custody as an anchor.
  A retired stream accepts no further `advance`.
- **`latest(streamId)`** returns the stream's highest accepted anchor, its retirement, **absent**,
  or **unavailable**.
- **`streams()`** returns the provider's **head**, which is the highest counter it has assigned, and
  every stream identity it holds an anchor or a retirement for, each with that anchor or retirement.
  The answer may also be **unavailable**. The head is kept by the provider as a value of its own.
  It is not computed from the surviving entries, so it never decreases, even after a retirement.
  A verifier uses this list to find streams that are missing from the log entirely.

A provider is **eligible** only if all of the following hold:

1. **Independent custody.** The writer of the mutable log cannot rewrite or delete what the provider
   has accepted, and cannot return the provider to an earlier state, using the access it uses to
   write the log. A provider that this test would disqualify is not an anchor, whatever its name.
2. **Monotonic acceptance.** The provider enforces the `positionNotIncreasing` and `conflict`
   refusals itself, never lowers its head, and never assigns a counter twice. It does not depend on
   the caller to behave.
3. **Faithful reads.** `latest` and `streams` return what the provider accepted or retired, or
   **unavailable**. They never return a value fabricated from the log.
4. **Retirements are kept.** A provider keeps every retirement for its own lifetime, and drops a
   stream's anchor only by replacing it with a retirement. A provider that cannot keep retirements
   may still serve as an anchor, but it must not drop anchors. Retirements are small: one per
   stream instance, the same size as an anchor. Issue #87 may define a compaction only if the
   compaction keeps an aged-out stream distinguishable from a deleted one.
5. **Authority over `advance` and `retire`.** Only the adapter, inside the trust boundary, can
   invoke `advance` and `retire`. An adversary who can rewrite the log's storage cannot invoke them,
   either directly or by getting the adapter to relay a claim built from storage it has rewritten
   (7.3). The integrator demonstrates this condition for each deployment. The library cannot
   establish it. A provider with its own clock may also refuse a `retire` before a declared minimum
   retention period has elapsed. That narrows the reliance on this condition without removing it.

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

- **What may be anchored.** An anchor's position never exceeds the stream's **durable position**:
  the highest record the storage contract reports as **durably confirmed** (ADR-002 level 3,
  Decision 9.3). Without this rule, power loss after anchoring but before storage would turn into a
  false truncation finding. Until that contract is accepted and implemented on an eligible backend
  (9.2), no anchor may be advanced, because nothing is durably confirmed.
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
  are issue #87. When a stream's records age out, the adapter calls `retire`, and the retirement
  replaces the anchor (7.2). The adapter calls it **after** the records are removed, never before,
  so an interrupted retention leaves the stream anchored rather than falsely retired. Absent a
  retirement, a stream that has disappeared is a finding, never expiry (7.4, 7.5). An expiry is
  legitimate only when an independently held retirement attests it.
  How a stream is verified while only part of it has aged out is a rotation-boundary question for
  issue #87. This record fixes only the two endpoints: all records present, or none.
- **Claim construction and resumption.** An adapter builds a claim only from the chain state it
  computed itself while writing that stream instance's records. It never builds a claim from chain
  state read back from the mutable storage. An adapter that resumes from storage first checks
  continuity. It recomputes `H_p` from the stored records up to the provider's latest position `p`
  for that stream, and compares the result with the provider's digest. Resumption happens after a
  restart, when chain state is recovered (issue #87), or before a `retire`. The adapter performs
  this check before any `advance`, and before any link that cites the old chain. If the digests
  differ, the adapter advances nothing, reports an integrity fault through audit health, and leaves
  the evidence untouched for the verifier. Records of an earlier stream instance past its last
  anchor are never anchored after resumption, because that would launder the exposure window. They
  stay internally consistent and unanchored. A restart starts a new stream instance in any case
  (ADR-002 Decision 5), so no live stream needs an anchor built from reloaded state.
- **Authority to expire.** `retire` is the adapter's to call, under the retention policy only
  (issue #87), and within the same trust boundary as `advance` (7.2, condition 5). A retirement
  issued with that authority from outside the policy is indistinguishable from a legitimate expiry.
  That is the same limit as for `advance`, and it is stated in 7.5.
- **Absence.** The provider holds neither an anchor nor a retirement for the stream, because none was
  ever accepted. The verifier reports the stream as **unanchored**. An anchor the provider holds but
  the verifier cannot use is not absence (7.1).
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

- **What is retained.** Two separate values, which are never compared with each other:
  - for each `providerId`, the **retained head**: the highest provider head seen;
  - for each `streamId`, the **retained anchor**: the `position`, `digest` and `counter` of the
    highest anchor verified for that stream, plus whether that stream had been retired.
  The provider's head advances with every stream. A stream's anchor counter advances only when that
  stream advances. Comparing one stream's anchor counter with the provider head would therefore
  report a rollback for every stream that was merely idle while another advanced.
- **Who retains it.** Either the **reader**, meaning the verification tooling, kept off the device
  and outside the log writer's authority, or an **independent witness**. A remote-witness provider
  already holds this state. A tamper-resistant counter on the device holds the head but not the
  per-stream part, so the reader must keep that part. State the device restores together with its
  log is excluded for the same reason anchors on the log's medium are (7.2).
- **When it is updated.** Only after a verification that finds neither a mismatch nor a conflict, and
  only by raising values, never by lowering them. A failed verification leaves the retained position
  unchanged, so a rollback that has been reported cannot be accepted silently afterwards.
- **What it detects.** The provider, or one of its streams, is **rolled back** when any of the
  following holds:
  - **Provider head.** The head from `streams()` is below the retained head for that provider.
    This is the only check that uses the retained head.
  - **Stream anchor.** The stream's anchor, or its retirement's final anchor, has a `counter` or a
    `position` below the retained anchor. The rollback finding also covers the same position with a
    different digest.
  - **Retirement undone.** The retained anchor records the stream as retired, but the provider holds
    an anchor for it again instead of the retirement.
  - **Stream missing.** A stream in the retained state has neither an anchor nor a retirement at the
    provider. Expiry never looks like this, because a legitimately aged-out stream carries a
    retirement (7.3).
  An anchor whose counter is below the provider head, while the stream's own counter and position
  are not below its retained anchor, is **not** a rollback. It is a stream that has not advanced
  since.
- **Re-verifying the retained checkpoint.** For a stream with a retained anchor `(p_r, H_r)`, the
  verifier recomputes `H_{p_r}` from the log it is given and compares the result with `H_r`, whatever
  position the current anchor has. A mismatch is reported as **Altered**, not as a rollback. A log
  that ends before `p_r` is reported as **Incomplete**. Comparing positions alone would miss a
  history that was rewritten and then re-anchored at some `q > p_r`. This check protects the prefix
  up to `p_r` even when the authority of 7.2, condition 5, was compromised after that verification.
  It does not apply to a retired stream whose records are gone.
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

For a retired stream, the retirement's final `position` and `digest` act as the anchor for every
check below. The list refers to verdicts by name. The order only decides which verdict is reported
when several apply.

1. **Inconsistent**: a record carries version 0, or a version other than the stream's version
   (8.3, steps 1 and 2), whatever versions the verifier knows. Or, under a stream version the
   verifier knows, a chain link fails between two records present, or a record is malformed or out
   of chain order (8.3, 8.4). The report names the first failing record.
2. **Cannot verify**: the provider holds an anchor for the stream, but the anchor is unusable
   (7.1): its `anchorFormat` or `canonicalVersion` is unknown, or a mandatory field is missing. The
   same verdict applies when the stream's version is one the verifier does not know (8.3, step 3)
   and no record is Inconsistent under steps 1 and 2. This verdict is reached before any check that
   reads the anchor. For an unusable anchor, the coverage is internal consistency only. For an
   unknown stream version there is no coverage at all, because the version fixes the digest. The
   report names the unknown version or the missing field. This is the only verdict for an unusable
   anchor.
3. **Rolled back**: a check of 7.4 fails.
4. **Conflict**: two anchors for the stream conflict (7.1).
5. **Altered**: the recomputed `H_p` differs from the anchor's `digest`, or the recomputed digest at
   the retained checkpoint differs from the retained digest (7.4). Some record at or before that
   position was changed, or a record was inserted or removed at or before it. The report names
   which comparison failed.
6. **Retired**: the stream is retired and the log holds none of its records. The records aged out
   under retention, through position `p`. There is nothing left to verify, and that is not a
   finding.
7. **Incomplete**: the last record present has a sequence `m < p`, or `m` is below the retained
   checkpoint `p_r` (7.4). Records `m+1 … p` are missing. The
   prefix is internally consistent but cannot be matched against `digest`, because `H_p` cannot be
   computed. A stream the provider holds an anchor for, and the log holds no record of, is
   Incomplete with `m = 0`.
8. **Anchored**: `H_p` matches. The report states `p`, and lists records past `p` as internally
   consistent and unanchored. When no retained position existed, it adds "rollback not excluded".
9. **Unanchored** or **anchor unavailable**: "internally consistent, unanchored", together with the
   cause.

No result is called "verified" unless the verdict is **Anchored**, and an Anchored verdict always
carries its position. The word never appears without the range it applies to.

**Limits this record states rather than hides.** An alteration or truncation that affects only
records **after** the last anchor cannot be detected. A rewriting adversary can reproduce internal
consistency there, and a truncated suffix cannot be told apart from records never written. The
exposure window of 7.3 bounds this limit. Nothing removes it. A verifier with no retained position
cannot exclude a joint rollback of log and anchor. An eligible provider that is itself compromised
defeats the mechanism. So does a compromise of the authority to `advance` or `retire` (7.2,
condition 5), which is outside the covered model. Whoever holds that authority can re-anchor a
rewritten history, or present the removal of evidence as routine retention, and the latest anchor
then attests the substitute. Only a checkpoint the reader retained earlier still protects its prefix.
A verifier does not report such a compromise. It reports what the anchors attest. Choosing a provider fit for the device's threat model is part of the
manufacturer's risk file, not the library's.

### 8. The canonical byte contract

This decision answers the second blocking question of Decision 4. It fixes the bytes of a record,
the digest, and the chain input, so that two implementations following this text produce the same
bytes and the same digests for the same `AuditEvent`. It is a specification. It writes no record and
claims no capability (Decision 6).

#### 8.1 The contract version

- **One number names the whole contract.** The contract version is an unsigned 16-bit value and
  the first two bytes of every record. This decision defines **version 1**. A version fixes the
  record encoding (8.2), the reading rules (8.3), the digest algorithm, the chain construction and
  the initial chain value (8.4). Changing any of them makes a new version (8.5). The anchor's
  `canonicalVersion` (7.1) carries this number.
- **Version 0 is never valid.** No writer produces it, so a record carrying it is corrupt, not of
  an unsupported version (8.3). A version number is never reused for a different contract.
- **One stream instance, one version.** The writer fixes the version when a stream instance begins,
  and every record of that instance carries it. A version changes only at a new stream instance,
  that is, at a restart (ADR-002 Decision 5). The **stream's version** is therefore the version of
  its first record present, and a record carrying any other version is out of chain order (8.4).

#### 8.2 Encoding of a record, version 1

The canonical bytes `C` of a record are the concatenation of the fields below, in this order, with
nothing before, between or after them. The primitive encodings are:

| Encoding | Bytes |
|---|---|
| `u8`, `u16`, `u64` | Unsigned integer, big-endian, fixed width of 1, 2 or 8 bytes. |
| `i64` | Signed integer, two's complement, big-endian, 8 bytes. |
| `string` | A `u16` byte count, then exactly that many bytes, copied as stored. No terminator, no padding, no Unicode normalization, no case folding, no trimming. |
| `enum` | One byte, taken from the tables below **by name**. The in-memory enumerator value is never cast into the record. `0x00` is never a valid enum byte. |
| `flag` | One byte: `0x00` false, `0x01` true. |
| `presence` | One byte: `0x00` absent, and nothing follows; `0x01` present, and the value follows. |

| # | Field | Encoding | Source in `AuditEvent` | Constraint a reader checks (8.3) |
|---|---|---|---|---|
| 1 | contract version | `u16` | — | `1` for this decision |
| 2 | `streamId` | `string` | `streamId()` | 1 … 96 bytes, identifier grammar |
| 3 | `sequence` | `u64` | `sequence()` | `≥ 1` |
| 4 | `category` | `enum` | `category()` | table below |
| 5 | `phase` | `enum` | `phase()` | table below |
| 6 | `time` | `presence`, then `i64` | `time()` | see below |
| 7 | `action` | `string` | `action()` | 1 … 64 bytes, identifier grammar |
| 8 | `actor` | `string` | `actor()` | 0 … 64 bytes, identifier grammar |
| 9 | `target` | `string` | `target()` | 1 … 96 bytes, identifier grammar |
| 10 | `requirementRef` | `string` | `requirementRef()` | 0 … 64 bytes, identifier grammar |
| 11 | `riskRef` | `string` | `riskRef()` | 0 … 64 bytes, identifier grammar |
| 12 | `correlationId` | `string` | `correlationId()` | 0 … 117 bytes, identifier grammar |
| 13 | `sourceSequence` | `presence`, then `u64` | `sourceSequence()` | any value when present, `0` included |
| 14 | `detail` | `string` | `detail()` | 0 … 160 bytes, any byte values |
| 15 | `detailTruncated` | `flag` | `detailTruncated()` | `0x00` or `0x01` |

The identifier grammar is the one `AuditEvent` enforces at admission: ASCII letters, digits,
underscore, dot, colon, slash and hyphen. The capacities are those of `AuditEvent` at the time of
this decision. Changing one is a new version (8.5).

| `category` | Byte | | `phase` | Byte |
|---|---|---|---|---|
| `Lifecycle` | `0x01` | | `Requested` | `0x01` |
| `Configuration` | `0x02` | | `Confirmed` | `0x02` |
| `Access` | `0x03` | | `Executed` | `0x03` |
| `RiskControl` | `0x04` | | `Failed` | `0x04` |
| `Operator` | `0x05` | | | |

- **Time.** An unavailable `RawTime` is encoded as the single byte `0x00`. The timestamp an
  unavailable `RawTime` holds in memory is not encoded. An available `RawTime` is `0x01` followed by
  its value as an `i64`: nanoseconds since the Unix epoch, as held by `RawTime`, with no conversion,
  rounding or leap-second adjustment. ADR-001 Decision 7 checks no plausibility, so zero and
  negative values are valid and encoded as they are (vectors V3 and V4). Version 1 requires the
  value to fit in 64 signed bits, which is the representation of `std::chrono::nanoseconds` on the
  supported toolchains.
- **Absent, empty, and zero.** Two kinds of optional value exist, and the encoding keeps each one
  exactly as the model has it:
  - `time` and `sourceSequence` have a presence state. A presence byte precedes the value, so an
    absent value (`0x00`) and a present value of zero (`0x01` then zero bytes) are different bytes
    (vectors V2 and V3).
  - `actor`, `requirementRef`, `riskRef` and `correlationId` have no absent state separate from
    empty. In `AuditEvent` an empty value is how the producer says "not supplied", and it is
    encoded as a zero byte count. That is the only encoding of this state. If a later model has to
    tell a supplied empty value from absence, that is a new version (8.5).
  - `detail` is not optional. It may be empty, which is a zero byte count.
- **Detail and its truncation.** The encoder writes the stored `detail` bytes and the stored
  `detailTruncated` flag. It never truncates, re-truncates, validates or repairs them. Truncation
  happened at admission (ADR-001 Decision 2), and the flag records it. The flag is part of the
  hashed bytes, so clearing it later breaks the chain. Neither the encoder nor the reader checks
  that `detail` is valid UTF-8.
- **Every field, and nothing else.** The fifteen fields cover every value `AuditEvent` holds,
  including its provenance: `sourceSequence` and `detailTruncated`. The canonical bytes contain no
  digest, no storage offset, no frame length and no signature. The admission result
  (`AuditWriteResult`) is not part of the record.
- **Self-delimiting.** A reader that knows version 1 finds the end of a record from its own bytes.
  How records are framed in storage is Decision 9 (9.4).

#### 8.3 Reading a record

A reader checks the stream in three steps, in this order. The first two need no knowledge of any
version, so their findings take precedence over an unknown version.

1. **Version 0.** A record shorter than the two version bytes, or whose version is 0, is
   **malformed**, whatever versions the reader knows (8.1).
2. **Version change.** A record whose version differs from the stream's version (8.1) is out of
   chain order (8.4). This holds whether or not the reader knows either version: a stream that
   starts in version 1 and then carries a version 2 is a chain failure at the first version-2
   record, even for a reader that knows only version 1.
3. **Unknown stream version.** If the reader does not know the stream's version, it decodes no
   record. The version also fixes the digest, so the reader cannot check the links either. The
   verifier reports **Cannot verify** and names the version (7.5). It never skips a record, and it
   never guesses another version.

Under a stream version it knows, a reader decodes each record and also finds it **malformed** if
any of the following holds:

- the bytes end before field 15;
- bytes remain after field 15, within the record's storage frame (9.4);
- an enum byte is not in its table;
- a presence or flag byte is neither `0x00` nor `0x01`;
- a byte count exceeds the field's capacity;
- an identifier field holds a byte outside the grammar;
- `streamId`, `action` or `target` is empty;
- `sequence` is zero.

Each field has exactly one encoding. Decoding a well-formed record and encoding it again therefore
gives back its bytes, and a reader may use that as its check.

Steps 1 and 2 and these decoding failures are reported as **Inconsistent** (8.4, 7.5). Step 3 is
reported as **Cannot verify**. Inconsistent comes first in 7.5, so a stream with both findings is
reported as Inconsistent, naming the first failing record.

#### 8.4 Digest and chain

- **Algorithm.** SHA-256 as specified in FIPS 180-4. Its output is 32 bytes.
- **Chain input.** For the records of one stream instance, with `C_k` the canonical bytes of the
  record whose `sequence` is `k`:

  ```text
  H_0 = 32 bytes of 0x00
  H_k = SHA-256( C_k ‖ H_{k−1} )      for k ≥ 1
  ```

  `‖` is plain concatenation, and `H_{k−1}` enters as its 32 raw bytes. No separator and no
  length prefix are added. The input is unambiguous because `H_{k−1}` has a fixed size and comes
  last.
- **Chain order.** The `k`-th record of a stream instance has `sequence = k` and that instance's
  `streamId`. A verifier treats any of the following as a chain failure at that record, reported
  as **Inconsistent** (7.5): a stored digest that differs from the recomputed `H_k`, a record whose
  `sequence` or `streamId` breaks this order, a malformed record (8.3, including version 0), or a
  record whose version differs from the stream's version (8.1). The last two are found before, and
  regardless of, any unknown version (8.3).
- **One initial value.** `H_0` is the same for every stream. Each chain is scoped by the `streamId`
  inside every record. A link from a new stream instance to the previous one (issue #87) is carried
  in records, never by changing `H_0`.
- **Hashed as stored.** A verifier hashes the record bytes as stored. It never hashes a re-encoding
  of the decoded fields. This is what keeps records of an older version verifiable (8.5).
- **Representation.** Inside the chain input, and wherever a digest is stored next to its record,
  a digest is its 32 raw bytes. In an anchor's `digest` (7.1), in a verifier's report and in any
  text, it is written as 64 lowercase hexadecimal characters, with no prefix and no separator. A
  verifier compares digests as bytes, after decoding the text.

#### 8.5 Evolution

- **Any change is a new version.** Adding, removing or reordering a field, changing a capacity or
  the identifier grammar, adding an enum value, or changing the digest or the chain makes a new
  version. ADR-002 Decision 6 keeps the category set provisional, so a new category is a new
  version. Once a stored record uses a version, that version's text and its vectors never change.
- **Unknown version.** A verifier that does not know a stream's version reports **Cannot verify**
  for the stream (8.3, step 3; 7.5). Version 0 and a version change inside a stream are not
  unknown versions: they are Inconsistent (8.3, steps 1 and 2). A verifier keeps every version it
  has supported. If it drops one that stored records still use, those records become unverifiable,
  and the verifier reports that.
- **A field added later.** Stored records are never re-encoded, upgraded or re-hashed. Each one
  keeps its version and is verified under that version's rules. A field added in version `N + 1`
  is not part of a version-`N` record: a report shows it as "not present in version N", never as
  a default value. Re-encoding a stored record would change every later digest and break every
  anchor past it. A verifier could not tell that from a rewrite (Decision 1), which is what the
  chain exists to reveal.

#### 8.6 Room for a signature

Decision 3 requires room for a future signature over the chain head. Version 1 leaves that room
**outside** the canonical bytes and outside the chain input. No record field, no byte of `C_k` and
no part of the input to `H_k` is reserved for a signature, and none would change when one is added.
A future signature would cover a head statement holding at least the contract version, the
`streamId`, a position `p` and `H_p`, which is the claim of 7.1. That later decision specifies the
statement's encoding, the algorithm and the key. It extends the anchor format or defines a separate
object, never this contract. Records written under version 1 therefore remain valid and verifiable
after signing is introduced.

#### 8.7 Reference vectors

Four records form one stream instance, `device-42/boot-7`, with sequences 1 to 4. Each block lists
the fields in order, as hexadecimal bytes. `C_k` is the concatenation of the bytes in the block,
without spaces, field names or comments. `61 × 159` means the byte `0x61` repeated 159 times. The
digests chain from `H_0` (8.4). Two independent implementations of this text computed the vectors
and agree on every byte and digest.

| Vector | What it fixes |
|---|---|
| V1 | Every optional value present; time available; detail not truncated. |
| V2 | Optional identifiers empty; `sourceSequence` absent; time unavailable; empty detail. |
| V3 | Detail truncated: the producer passed 161 bytes, 159 bytes `a` and then `é` (`c3 a9`). Admission cut it at 159 bytes, before the two-byte sequence, under ADR-001 Decision 2, and set the flag. `sourceSequence` and time are present with the value zero. |
| V4 | Negative time; the largest `sourceSequence`; a non-ASCII detail that is not truncated. |

**V1** — `|C_1|` = 157 bytes.

```text
version          00 01
streamId         00 10 64 65 76 69 63 65 2d 34 32 2f 62 6f 6f 74 2d 37   ; "device-42/boot-7"
sequence         00 00 00 00 00 00 00 01                                  ; 1
category         02                                                       ; Configuration
phase            03                                                       ; Executed
time             01 18 86 72 51 f5 55 cd 15                               ; available, 1767225600123456789 ns
action           00 10 74 68 65 72 61 70 79 2e 72 61 74 65 2e 73 65 74   ; "therapy.rate.set"
actor            00 11 6f 70 65 72 61 74 6f 72 3a 6e 75 72 73 65 2d 30 37
                                                                          ; "operator:nurse-07"
target           00 0e 70 75 6d 70 2f 63 68 61 6e 6e 65 6c 2d 41         ; "pump/channel-A"
requirementRef   00 0b 52 45 51 2d 41 4c 4d 2d 30 31 32                   ; "REQ-ALM-012"
riskRef          00 05 52 43 2d 31 37                                     ; "RC-17"
correlationId    00 07 6f 70 2d 30 30 30 31                               ; "op-0001"
sourceSequence   01 00 00 00 00 00 00 00 29                               ; present, 41
detail           00 18 72 61 74 65 20 31 32 2e 35 20 6d 4c 2f 68 20 63 6f 6e 66 69 72 6d 65 64
                                                                          ; "rate 12.5 mL/h confirmed"
detailTruncated  00                                                       ; false

H_1 = 5384ec4133d6baab7790b48a0fa0c8eb3d249e37d9487a886aa47b09803b9055
```

**V2** — `|C_2|` = 68 bytes.

```text
version          00 01
streamId         00 10 64 65 76 69 63 65 2d 34 32 2f 62 6f 6f 74 2d 37   ; "device-42/boot-7"
sequence         00 00 00 00 00 00 00 02                                  ; 2
category         01                                                       ; Lifecycle
phase            01                                                       ; Requested
time             00                                                       ; unavailable
action           00 0c 64 65 76 69 63 65 2e 73 74 61 72 74               ; "device.start"
actor            00 00                                                    ; empty
target           00 09 64 65 76 69 63 65 2d 34 32                         ; "device-42"
requirementRef   00 00                                                    ; empty
riskRef          00 00                                                    ; empty
correlationId    00 00                                                    ; empty
sourceSequence   00                                                       ; absent
detail           00 00                                                    ; empty
detailTruncated  00                                                       ; false

H_2 = dd894a8130712adfc820f13daf1bc72f68ba701d7bf4f6cbd2b8ff31f5591fd8
```

**V3** — `|C_3|` = 251 bytes.

```text
version          00 01
streamId         00 10 64 65 76 69 63 65 2d 34 32 2f 62 6f 6f 74 2d 37   ; "device-42/boot-7"
sequence         00 00 00 00 00 00 00 03                                  ; 3
category         04                                                       ; RiskControl
phase            04                                                       ; Failed
time             01 00 00 00 00 00 00 00 00                               ; available, 0 ns
action           00 09 61 6c 61 72 6d 2e 61 63 6b                         ; "alarm.ack"
actor            00 00                                                    ; empty
target           00 0f 61 6c 61 72 6d 2f 6f 63 63 6c 75 73 69 6f 6e      ; "alarm/occlusion"
requirementRef   00 00                                                    ; empty
riskRef          00 05 52 43 2d 30 33                                     ; "RC-03"
correlationId    00 00                                                    ; empty
sourceSequence   01 00 00 00 00 00 00 00 00                               ; present, 0
detail           00 9f 61 × 159                                           ; 159 bytes "a"
detailTruncated  01                                                       ; true

H_3 = 0a870f567b1cb9781ebde3d8bdc9d388d463ee6c38fe660256cf36e9da147027
```

**V4** — `|C_4|` = 98 bytes.

```text
version          00 01
streamId         00 10 64 65 76 69 63 65 2d 34 32 2f 62 6f 6f 74 2d 37   ; "device-42/boot-7"
sequence         00 00 00 00 00 00 00 04                                  ; 4
category         03                                                       ; Access
phase            02                                                       ; Confirmed
time             01 ff ff ff ff ff ff ff ff                               ; available, -1 ns
action           00 0d 73 65 73 73 69 6f 6e 2e 6c 6f 67 69 6e            ; "session.login"
actor            00 0b 73 76 63 3a 75 70 64 61 74 65 72                   ; "svc:updater"
target           00 09 73 65 73 73 69 6f 6e 2f 33                         ; "session/3"
requirementRef   00 00                                                    ; empty
riskRef          00 00                                                    ; empty
correlationId    00 00                                                    ; empty
sourceSequence   01 ff ff ff ff ff ff ff ff                               ; present, 18446744073709551615
detail           00 02 c3 a9                                              ; "é"
detailTruncated  00                                                       ; false

H_4 = 12d523bdf4082156aecfb31c96af83a80054383470b9b06f7e6b5be447c42b07
```

### 9. Storage, power-loss atomicity, and what "durably confirmed" means

This decision answers two questions of Decision 4: the storage medium and layout, and power-loss
atomicity. It fixes the layout of the stored bytes, the storage abstraction the adapter writes
through, the exact point at which a record becomes **durably confirmed** (ADR-002 Decision 3,
level 3), what every power cut leaves behind, and how storage failures reach the audit health
signal. It is a specification. It writes no record, and level 3 stays unclaimed until an
implementation exists and is verified (Decision 6).

Storage is an adapter concern (ADR-001). Nothing here moves I/O, flushing or recovery into the
governed core. The records written are the canonical bytes and digests of the canonical byte
contract (Decision 8, issue #84). This decision frames them and never interprets them.

#### 9.1 Medium and layout: append-only segments, on any eligible medium

The record chooses a **layout**, not a medium. The stored log is a set of **segments**. A segment is
a bounded byte region that is written only by appending, and is never rewritten in place. It is
removed only whole, by retention (issue #87). Each segment holds records of exactly one stream
instance (ADR-002 Decision 5). A stream instance spans one or more segments, numbered from 0
without gaps.

The three media Decision 4 named are all usable, under this layout, and only under it:

- **Segmented files**: one segment is one file, created exclusively, and appended to.
- **A circular region of flash**: the region is divided into segments of whole erase blocks. The
  circle turns **one segment at a time**. A segment is erased only whole, and only when retention
  reclaims it. Erasing in whole segments also spreads wear across the region in order.
- **An append-only device log**: one segment is one range of the device log, if the device gives
  the acknowledgment 9.2 requires.

**Rejected: overwriting the oldest records in place**, at record or page granularity. A cut during
such an overwrite leaves a half-erased old record next to a half-written new one, and the medium
then holds bytes that belong to no single state of the log. Ageing out is a retention decision
(issue #87), taken one whole segment at a time.

Which medium a deployment uses, and the size of a segment, are the integrator's choice. They
declare both as configuration. The wear and write-amplification trade-off of the sync policy (9.3)
is also theirs: every sync may program a partial page.

#### 9.2 The storage abstraction and its minimal contract

The adapter writes through the operations below. Like 7.2, they are an interface contract, not C++
signatures; the implementing issue writes those.

- **`open(streamId, segmentIndex, firstSequence)`** creates a segment and appends its preamble and
  header frame (9.4). It answers **opened**, **noSpace** or **failed**. Like `append`, **opened
  means nothing about durability**: the preamble and the header become durable through the first
  `sync` that covers them (below), and only on an eligible backend.
- **`append(segment, bytes)`** places bytes after the segment's current end. It answers **written**,
  with the new end offset, or **failed**. **Written means nothing about durability.** The bytes may
  still sit in a cache, a buffer or a controller.
- **`sync(segment, offset)`** answers **durable** only when every byte of the segment before
  `offset`, the preamble and header included, will survive a power loss at any later instant, and
  so will the segment's existence, such as a new file's directory entry. On an eligible backend
  (below), it answers **durable** or **failed**. A backend that is not eligible answers
  **unsupported**, every time, and never durable. Unsupported is not a failure (9.6). There is no
  optimistic answer.
- **`read(segment, offset, length)`** and **`segments()`** serve recovery and verification. They
  return what the medium holds, or **unavailable**.
- **`reclaim(segment)`** removes a whole segment. Only retention calls it (issue #87).

A storage backend is **eligible for level 3** only if all of the following hold. As for the
providers of 7.2, the integrator demonstrates them for each deployment, and the library cannot.

1. **A truthful barrier.** `sync` answers **durable** only after the medium itself has acknowledged
   persistence. A volatile write cache that a power loss empties, or a flush that returns before the
   medium has persisted the data, disqualifies the backend. Examples of what the integrator must
   check: on Linux, `fsync` on the file, plus the directory when a segment file is created; on
   macOS, `F_FULLFSYNC`, because `fsync` there does not flush the drive's cache; on raw flash, the
   end of the program operation as the controller reports it.
2. **A stable prefix.** Bytes of a segment before the last offset `sync` confirmed are never
   altered by a later `append`, `sync` or power loss. A medium where an interrupted program can
   corrupt data programmed earlier, as with paired pages on some NAND flash, is eligible only if
   its backend prevents that, for example by padding to the safe boundary before each sync.
3. **Nothing past the barrier is promised.** After a power loss, the bytes after the last
   confirmed offset may be any mix of absent, partial, complete or erased. The layout (9.4) is
   designed to read that region safely. The backend need not order those bytes.

A backend that is not eligible may still store records. The integrator declares it as such in the
configuration. It opens segments and appends frames like any other, and its `sync` answers
**unsupported**. Nothing it stores is ever durably confirmed, its durable position stays 0, and no
anchor ever covers it (7.3).

#### 9.3 What "durably confirmed" means

Of the three readings Decision 4 offered (written, flushed, acknowledged by the medium), this
record takes the third.

> A record of sequence `n` in a stream instance is **durably confirmed** when `sync` has answered
> **durable** for an offset at or beyond the end of its frame (9.4), and every earlier record of
> that instance is durably confirmed.

- **The acknowledgment boundary is the return of `sync` with the answer durable.** The return of
  `append` is not a confirmation, and neither is a flush that was issued. A `sync` still in progress
  confirms nothing.
- **Confirmation is a prefix.** It advances in sequence order, and never past a record that is not
  confirmed. The adapter publishes it per stream instance as the **durable position**: the highest
  confirmed sequence, or 0. The durable position only increases.
- **Level 3 is not level 2.** `AuditSink::accept()` returning true still means hand-off: the
  persisting sink copied the event into its own bounded pending buffer. Durable confirmation is
  reported later, through the durable position (9.6), never through the producing call or through
  `accept()`.
- **No durable acknowledgment before the contract holds.** The adapter never publishes a durable
  position, and never offers an anchor claim (7.3), above what a `sync` answered durable for. A
  backend that is not eligible (9.2) publishes none.
- **Sync policy.** A `sync` per record is allowed but not required. The integrator declares a
  policy with a record bound and an age bound, as for anchors (7.3), plus a sync on orderly close.
  The adapter syncs on its consumer thread. Syncing never holds up admission: while it runs, the
  ring absorbs events, and a full ring refuses at the call site (ADR-002 Decision 3).
- **Retries.** `AuditSink` allows a retry of the same `(streamId, sequence)` (ADR-002 Decision 3).
  A retried event whose sequence was already appended, and whose canonical bytes are identical, is
  accepted without a second append. Different bytes under the same sequence are an integrity fault
  (9.6). An event whose sequence is not the next one of its instance is a fault too, because the
  chain admits no gap (Decision 8).

#### 9.4 Layout of the stored bytes, layout version 1

All integers are big-endian, as in Decision 8. Version 1 of this layout is independent of the
contract version of Decision 8: a segment can only hold records of one contract version, because
it holds one stream instance (Decision 8).

**A segment** is a **preamble**, then **frames** back to back, then unused space.

| Bytes | Field | Value |
|---|---|---|
| 4 | magic | `6d 64 6c 67` (`"mdlg"`) |
| 2 | layout version | `u16`, `1` for this decision. `0` is never valid. |

**A frame** is:

| Bytes | Field | Value |
|---|---|---|
| 1 | type | `0x01` segment header, `0x02` record. Every other value is invalid in layout version 1. |
| 4 | length | `u32`, the length of the payload. For type `0x01`, from 15 to 65 536. For type `0x02`, from 33 to 65 536. |
| length | payload | see below |
| 4 | check | `u32`, CRC-32C of the type, length and payload bytes |

- **The CRC.** CRC-32C (Castagnoli): reflected polynomial `0x82F63B78`, initial value
  `0xFFFFFFFF`, final XOR `0xFFFFFFFF`. The check value for the ASCII bytes `123456789` is
  `0xE3069283`. The check detects **accidental** damage, a torn write above all. It proves nothing
  against a deliberate rewrite: anyone can recompute it. Tamper evidence is the chain's job and the
  anchor's (Decision 1).
- **Header frame (type `0x01`).** The first frame of every segment, and only there. Its payload is
  `segmentIndex` (`u32`, from 0), `firstSequence` (`u64`, the sequence the first record of this
  segment will carry) and `streamId`, encoded as a Decision 8 `string`.
- **Record frame (type `0x02`).** Its payload is `C_k ‖ H_k`: the canonical bytes of the record
  (Decision 8), then its 32-byte chain digest. `C_k` takes the first `length − 32` bytes. Bytes left
  after field 15 of `C_k` make the record malformed (Decision 8).
- **Why types and bounds.** An erased flash byte (`0xFF`) and a zeroed byte (`0x00`) are both
  invalid types, and an erased or zeroed length is out of bounds. Unused space therefore never
  reads as a frame. The bounds do not depend on the contract version.
- **Evolution.** Any change to the preamble, a frame type, a payload or a bound is a new layout
  version. A reader that does not know a segment's layout version reads nothing from it and reports
  **Cannot verify** for the stream, naming the version, as Decision 8 does for an unknown contract
  version.

**Recognising a partial record.** A reader scans a segment from the end of the preamble. The bytes
at offset `o` are a **valid frame** when all of the following hold. Otherwise the scan stops at `o`:

1. the 5 bytes of type and length lie inside what the medium returns;
2. the type is `0x01` at the first frame, and `0x02` after it;
3. the length is within the bounds of its type;
4. the whole frame, check included, lies inside what the medium returns;
5. the check equals the CRC-32C the reader computes.

The bytes from `o` to the end of the segment are the segment's **trailing bytes**. The reader never
parses a record from them, never repairs them, and never resynchronises inside them, because a
payload may contain anything. A record is in the log only if its frame is valid.

**Vectors.** The first segment of the stream instance `device-42/boot-7`, holding the record of
vector V1 (Decision 8):

```text
offset 0    preamble       6d 64 6c 67 00 01
offset 6    header frame   01                                           ; type: segment header
                           00 00 00 1e                                  ; length 30
                           00 00 00 00                                  ; segmentIndex 0
                           00 00 00 00 00 00 00 01                      ; firstSequence 1
                           00 10 64 65 76 69 63 65 2d 34 32 2f 62 6f 6f 74 2d 37
                                                                        ; "device-42/boot-7"
                           78 0d 8e 4f                                  ; check
offset 45   record frame   02                                           ; type: record
                           00 00 00 bd                                  ; length 189 = 157 + 32
                           C_1                                          ; 157 bytes, vector V1
                           53 84 ec 41 33 d6 ba ab 77 90 b4 8a 0f a0 c8 eb
                           3d 24 9e 37 d9 48 7a 88 6a a4 7b 09 80 3b 90 55
                                                                        ; H_1
                           99 90 ad 35                                  ; check
offset 243  end of the last frame
```

If a cut leaves that segment ending at an offset `e` with `45 < e < 243`, the record frame fails
rule 1 (`e < 50`) or rule 4: the reader finds the header, no record, and `e − 45` trailing bytes
from offset 45. With `e ≥ 243`, the record is present. Changing any single byte of the record
frame other than its length makes rule 2 or rule 5 fail, because a CRC-32C detects every error
confined to 32 consecutive bits.

#### 9.5 Every power-cut point has a defined outcome

The table follows one record, of sequence `n`, through its write. "Before the cut" is what the host
had been told. "After restart" is what the medium may hold, and what a reader then reports. A
restart always starts a new stream instance (ADR-002 Decision 5), so no cut is ever "continued":
the old instance ends, and the discontinuity stays visible.

| Cut point | Before the cut | What the medium may hold, and what the reader reports |
|---|---|---|
| **Before the append** of frame `n` | At most handed off (level 2). Durable position below `n`. | No frame `n`. The stream ends at an earlier record, possibly with trailing bytes from an earlier unconfirmed frame. Record `n` is lost. Since no anchor exceeds the durable position (7.3), the loss produces no Incomplete finding, and nothing ever claimed `n` durable. |
| **During the append**, before it answers | Same. | Frame `n` absent, partial or complete. The cut may fall after the bytes were fully written, or even persisted, but before `append` returned. Absent or partial: the scan stops at its start (9.4) and reports trailing bytes there, from which no record is parsed. Complete with a valid check: it is a record, as in the next row. Neither case says anything about confirmation, which only a `sync` answer gives (9.3). |
| **After the append, before `sync`** | Same. `append` answered written, which promises nothing. | Frame `n` absent, partial or complete, and the same for every unconfirmed frame before it. A complete frame with a valid check is a record. It lies past the durable position, so past any anchor, and the verifier reports it internally consistent and unanchored (7.5). |
| **During `sync`**, before it answers | Same. | As in the previous row. The bytes may already be persisted when the cut falls, and the frame then reads as complete. A `sync` that has not answered confirms nothing, whatever the reader finds. |
| **After `sync` answered durable**, before the durable position was published | Not yet told. The record was confirmed in fact, but not reported. | Frame `n` is complete and valid, guaranteed by conditions 1 and 2 of 9.2. The host must treat a record it had no confirmation for as **unknown**, never as lost. |
| **After the durable position was published** | Durable through `n`. | Frame `n` is complete and valid. If it is missing or damaged, that is not a power-loss outcome but a medium fault or an alteration. Up to the last anchor, the verifier reports Incomplete, Inconsistent or Altered (7.5). Past it, the loss cannot be told from records never written, which is the exposure-window limit of 7.5. |
| **During `open`** of a new segment, before it answers | No record of the new segment was confirmed, since `append` follows `open` and a confirmation needs a `sync` covering the header. | The segment absent, or its preamble and header absent, partial or complete. Without a valid preamble and header frame, the reader reports a segment without a valid header and reads no record from it. With them, it is an empty segment, as in the next row. A valid header proves nothing about durability. |
| **After `open`, before the first `sync`** that covers the header | Same. | As in the previous row: the header may be absent, partial or complete, since `opened` promised nothing. A complete header with no record frame is an empty segment. This is normal. |

**What a reader concludes from trailing bytes.** In the **last** segment of a stream instance,
trailing bytes are the expected trace of a cut, and the reader reports their offset and length.
In any **earlier** segment, the bytes after the last valid frame are unused space if the next
segment's header carries `firstSequence` equal to that frame's sequence plus one, and the chain
continues across the boundary. Otherwise records are missing, and the chain check reports
**Inconsistent** at the first record that does not follow (Decision 8). Duplicate segment indices
with valid headers, a gap in segment indices, or a header whose `streamId` differs from its records
are also reported as Inconsistent: the adapter of 9.6 never produces them. These conclusions are
the verifier's, which reads every segment. The adapter's startup check covers only part of them
(9.6).

#### 9.6 Storage failures and the audit health signal

- **A failure ends durability for the stream instance.** If `open`, `append` or `sync` answers
  failed or noSpace (an **unsupported** `sync` from a declared non-eligible backend is not a
  failure), or the adapter detects a retry with different bytes or a sequence out of order
  (9.3), the adapter writes nothing more to that instance. It does not retry at the same offset,
  which could overwrite part of a frame, and it does not continue in a new segment, which would
  hide the damaged region. From then on, the persisting sink refuses events for that instance from
  `accept()`. They stay in the ring, the ring fills, and producers see refusals at the call site
  (ADR-002 Decision 3). The host decides whether to stop, degrade or restart. A restart starts a
  new instance, with the discontinuity visible.
- **What the health signal reports.** Alongside the counters of ADR-002 Decision 3, the adapter
  publishes, for each stream instance, its durable position and its storage state, **persisting**
  or **failed**, and, across instances, counters with the last cause: open, append and sync
  failures, no space, duplicate mismatch, and sequence out of order. Events the sink had accepted
  but never confirmed when an instance failed are counted as **not durable at failure**, and they
  are reported as losses after admission.
- **Recovery findings.** At startup, before a new instance writes, the adapter runs a bounded
  check and reports, through the same signal, exactly what that check covers:
  - for **every** segment, its preamble and header frame only: segments without a valid header,
    unknown layout versions, and, per stream instance, duplicate segment indices and gaps in them;
  - for the **last** segment of every earlier instance, a full frame scan (9.4): trailing bytes,
    with their offset and length.

  The check reads no record frame of an earlier segment. It recomputes no digest, does not check
  that `firstSequence` follows on at a segment boundary, and does not compare the `streamId` of a
  record with its header's. Those findings belong to the verifier (7.5), which reads every segment
  in full, and the adapter reports none of them. Recovering the chain state itself, and linking the
  new chain to the old one, is issue #87.
- **Never through the failing path.** The adapter never reports a storage failure as an audit
  event, never writes it to the storage that failed, and never sends it through any sink that
  writes to that storage. The health signal is read by the host, as for ADR-002 Decision 3. A host
  that records the failure as an audit event of its own does so knowingly, through the normal
  path, where that event may itself be refused.

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

### 4. Overwrite the oldest records in place, in one circular region (Rejected)
**Pros:** The simplest flash layout; no segment bookkeeping.
**Cons:** A cut during an overwrite leaves bytes that belong to no single state of the log, and
ageing out becomes a side effect of writing rather than a retention decision. Decision 9 keeps the
circle, but turns it one whole segment at a time.

### 5. Treat a successful write, or an issued flush, as durable (Rejected)
**Pros:** Confirmation at once, with no barrier and no sync policy.
**Cons:** Write caches and flushes that return early lose acknowledged data on power loss. An
anchor built on that confirmation would then report a truncation that never happened (7.3).
Decision 9 confirms only on the medium's acknowledgment, and makes a lying barrier a reason to
disqualify the backend.

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
- Every change to the record format is a new contract version (8.5), and a verifier keeps every
  version that stored records use. The format can grow, but never in place.
- Durable confirmation costs a `sync` on the persistence path, and on flash, write amplification.
  The sync policy trades that cost against how many handed-off records a power cut may lose (9.3).
- A storage failure ends durability for its stream instance (9.6). Until the host restarts it, the
  audit ring fills and producers are refused, by design, rather than continuing unpersisted.
- Three questions remain open (Decision 4). Both blocking ones are answered (Decisions 7 and 8),
  and Decision 9 defines durable confirmation, but retention, rotation and chain state recovery
  (#87) must still be settled before anything is stored. This
  record therefore cannot be implemented as it stands. It bounds the design rather than settling it.

### Risks and Mitigations
- **The record is read as a plan rather than a boundary.** *Mitigation*: Status and Decision 6 both
  say what it is; the index marks it Proposed like the rest.
- **The chain is mistaken for the whole mechanism.** This is not hypothetical — the previous
  revision of this record made exactly that error, claiming detection from chaining alone when a
  rewriting adversary defeats it by recomputation or truncation. *Mitigation*: Decision 1 states the
  conditional property and its scope, and Decision 5's first three scenarios fail any implementation
  that reproduces the error.
- **The latest anchor is trusted beyond its threat model.** An adversary who controls the adapter,
  or who gets it to relay a rewritten chain, can re-anchor that history. The provider of Decision 7
  does not check chain continuity. *Mitigation*: Decision 7 states the threat model, makes the
  authority over `advance` and `retire` an eligibility condition, requires a continuity check before
  any resumed adapter advances, and has the reader re-verify its retained checkpoint. A provider
  that keeps and exposes its full history is a stronger option, left to a separate decision.
- **A chain is treated as proof of authorship.** *Mitigation*: Decision 1 states what the claim is
  and is not, in the words to use.
- **Canonical serialization is re-derived badly, or decided late.** *Mitigation*: Decision 4 kept
  the byte contract blocking until Decision 8 specified it. Decision 8 fixes each field's encoding
  by name rather than by in-memory layout, and gives reference vectors that two independent
  implementations reproduced byte for byte.

## References
- ADR-002 Decision 3 and Decision 5 (this repository) — the delivery level this record would fill,
  and the stream identity/sequence a chain is scoped by.
- ADR-001 (this repository) — why key material and storage cannot be governed-zone concerns.
- [MduX ADR-007: Evidence pipeline doctrine](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/ADR-007-evidence-pipeline-doctrine.md) — canonical, byte-stable serialization and the determinism failure modes Decision 2 inherits.
- mddlog issue #5 — the explicit exclusion of persistent/cryptographically protected audit storage.
- [FIPS 180-4: Secure Hash Standard](https://csrc.nist.gov/pubs/fips/180-4/upd1/final) — SHA-256,
  the digest of Decision 8.
- mddlog issues #84 (canonical byte contract and its contract version, specified by Decision 8 and
  cited by `canonicalVersion`),
  #86 (storage and the meaning of durably confirmed, which bounds an anchor's position, specified
  by Decision 9) and #87
  (restart, rotation and retention, which bound how long anchors are kept). Decision 7 depends on
  all three. #90 implements the provider interface and verifier specified there.

## Approval
- **Decision Date**: not yet approved — drafted for review.
- **Approved By**: pending (project maintainer).
- **Review Date**: when Decision 4's open questions are answered, which is a precondition for any implementation work.
