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
  adversary can rewrite too.

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

These are unresolved. An implementation must not pick silently — and the first two are **blocking**:
nothing may be stored before they are answered, because the first record written freezes both.

- **The anchoring mechanism** (blocking, Decision 1) — what supplies the authentic anchor, how often
  it advances, where it is held, and what a reader is told when it is absent or stale. Without an
  answer, the only claim available is internal chain consistency.
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
| **Rewrite with recomputation** — a middle record is altered and every later digest recomputed | Mismatch against the anchor. Internal links alone verify, which is exactly why the anchor is required |
| **Suffix truncation** — the last *n* records are deleted | Coverage short of the anchor's position: the prefix is internally consistent and demonstrably incomplete |
| **Old log restored with its matching old anchor** — both rolled back together | Detected as **stale**, not valid: the anchor's position is behind the position the reader last observed, which is why an anchor carries a position and a reader must retain the highest one seen |
| **Missing anchor** | "Internally consistent, unanchored" — never "verified" |
| **Restart** — a new stream instance begins (ADR-002 Decision 5) | A new chain, explicitly linked to the previous stream identity, with the discontinuity visible rather than closed up |

The third scenario is the one that shows why an anchor must be more than a digest: a rollback of log
and anchor together is internally coherent, and only a monotonic position a reader remembers — or a
witness outside the device — distinguishes it from the truth.

### 6. Nothing about this record changes the claims allowed today

Until it is implemented and verified, ADR-002 Decision 3 stands unchanged: mddlog promises in-memory
admission only, and power loss is outside every guarantee. This record's existence is not evidence of
a capability, which is the same caution the index applies to every Proposed record.

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
- The detection property now requires an anchoring mechanism this record does not choose, so the
  useful claim depends on a question still open — which is more honest than the previous draft and
  strictly more work.
- Leaving seven questions open (Decision 4), two of them blocking, means this record cannot be
  implemented as it stands — it bounds the design rather than settling it.

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

## Approval
- **Decision Date**: not yet approved — drafted for review.
- **Approved By**: pending (project maintainer).
- **Review Date**: when Decision 4's open questions are answered, which is a precondition for any implementation work.
