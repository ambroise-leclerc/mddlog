# ADR-002: Regulatory audit-event model, separate from application logging

## Status
Proposed — drafted for maintainer review, not yet acted on.

## Context

`README.md` lists, under Medical Device Compliance: "Audit Trail: Tamper-proof logging with
cryptographic signatures", "Risk Management: Hazard tracking and mitigation logging per ISO 14971",
"Regulatory Reporting: Automated compliance report generation", and under Advanced Sink System an
`AuditSink` marked `(planned)`. None of this exists in the current module set. What exists instead:

- `AUDIT` is one more value of the `LogLevel` enum (`include/mddlog/core/LogLevel.cppm:24`), ordered
  as the *highest-severity* level, sitting above `FATAL`. Severity and audit-relevance are
  conflated: nothing distinguishes "this is unusually severe" from "this must be retained for
  regulatory purposes regardless of severity."
- `SimpleLogger::logAudit()` (`include/mddlog/core/Logger.cppm:133-141`) builds an ordinary
  `LogRecord` with `level = LogLevel::AUDIT`, sets three free-text fields
  (`auditEventType`, `riskLevel`, `complianceStandard` — `LogRecord.cppm:39-42`, defaulted to the
  literal string `"IEC_62304"` in `setAuditInfo()`), and pushes it through the exact same
  best-effort pipeline as a `trace()` call: the same unbounded queue, the same silently-swallowed
  sink exceptions (`writeToSinks()`, `Logger.cppm:269-280`), the same `shouldLog()` gate — meaning
  `logAudit()` is **not** exempt from `setMinLevel()` filtering or from `setEnabled(false)` today,
  even though nothing states whether that is intended.
- There is no schema, no requirement/hazard linkage, no cryptographic sealing, and no export path
  distinct from `ConsoleSink`'s human-readable line format (`sinks/ConsoleSink.cppm:64-99`).
- mddlog's own tracked work already names this gap without closing it: issue #5's "Audit policy" row
  asks to "Define and test whether `logAudit()` intentionally bypasses ordinary logger disablement/
  filtering" as a *specification* of current behavior, and its Boundaries section explicitly
  excludes "persistent/cryptographically protected audit storage" from that issue's scope. Both
  statements are consistent with there being no audit *model* yet to specify or persist — only a log
  level with a suggestive name.

This ADR does not implement cryptographic sealing or persistent storage — those remain out of scope
here too, for the same reason issue #5 gives: they are a distinct, larger concern (key management,
storage medium, tamper-evidence guarantees) that deserves its own ADR once this one settles what an
audit event *is*.

## Medical Device Considerations

### IEC 62304 / ISO 13485 / ISO 14971 implications
- **IEC 62304 §9 (problem resolution) and §5.8 (release)** both depend on being able to answer "what
  happened, when, and was it verified" from a record — the same question MduX's
  `mdux.governance.AuditEvent`/`ComplianceProgram` types exist to answer for that project's design
  history, generalized here to runtime events a deployed device produces.
- **ISO 14971 §7 (risk control) / §9 (post-market surveillance)**: a hazard-relevant runtime event
  (an alarm silenced, a safety interlock overridden, a configuration changed) is exactly the class of
  event a regulator or a post-market surveillance process expects to be able to reconstruct. A log
  line that can be filtered out by `setMinLevel()` or lost when a sink throws cannot serve that
  purpose, which is why "audit" cannot remain a `LogLevel` value subject to the same filtering as
  `TRACE`/`DEBUG`.
- **Fail-closed delivery, not fail-open severity.** The current design conflates "important enough
  to always show" (a severity concern) with "must never be silently dropped" (a delivery-guarantee
  concern). An audit event needs the second property; it does not need to be the numerically highest
  severity to need it — a `LogLevel::INFO`-severity "user acknowledged alarm" event is exactly as
  audit-relevant as a `FATAL` one, and today's ordering (`AUDIT` above `FATAL`) does not express
  that distinction at all.

### Risk management considerations
- Recording a fabricated or double-counted audit event is its own hazard (a device that claims an
  interlock was overridden with operator confirmation when it was not, or reports one confirmation
  twice, is worse than reporting the same event as an unremarkable log line would be). This is why
  Decision 3 below requires an audit event to identify what it attests to, not only that something
  happened.

## Decision

### 1. `AuditEvent` is a distinct type, not a `LogLevel` value

Remove `AUDIT` from the severity ordering's use as a filtering threshold (or keep the enumerator
for backward display purposes, but stop using it as `logAudit()`'s gate). Introduce a separate
`AuditEvent` type, shaped after
[MduX's `mdux.governance::AuditEvent`](https://github.com/ambroise-leclerc/MduX/blob/main/include/mdux/governance/Governance.cppm#L304)
but adapted for a runtime record instead of a design-history one:

```text
AuditCategory  { Lifecycle, Configuration, Access, RiskControl, Operator }
AuditEvent {
    category     AuditCategory
    timestamp    single ISO-8601 UTC spelling — one format, so two events order by string comparison
    subject      what the event is about (a control id, a user/session reference, a configuration key)
    outcome      what happened (e.g. Confirmed | Overridden | Failed) — an enum, not free text
    sequence     a monotonic counter, the same role as MduX's ActionTrace::sequence
}
```

Compare to MduX's precedent explicitly: `mdux.governance::AuditEvent` is a *design-history* record
(three categories: Lifecycle, Verification, Change) populated by a build-time compliance program,
and it allocates (`std::string` fields) because it lives in the `governed-throw` tier, not the
no-heap tier. mddlog's `AuditEvent` is a *runtime* record a deployed device produces, so it should
default to the fixed-capacity field shapes from ADR-001 rather than `std::string`, and it needs a
`sequence` field MduX's design-history record does not, for the same reason MduX added `sequence` to
`medui::ActionTrace` rather than to `governance::AuditEvent`: an ordered runtime stream needs a
tamper-evidence-adjacent ordering property a document-style record does not.

### 2. An audit event is never subject to the ordinary logger's severity filter or best-effort delivery

`logAudit()` (or its replacement) does not go through `shouldLog()`/`setMinLevel()`, and does not
share the unbounded `std::queue<LogRecord>` or the swallowed-exception path in `writeToSinks()`. This
directly answers issue #5's "Audit policy" question: **yes, intentionally bypasses** ordinary
filtering — because severity and audit-relevance are different axes (see Medical Device
Considerations), not because audit events are simply "more severe." What replaces best-effort
delivery is a delivery-outcome contract: recording an audit event returns whether it was accepted,
following the same bounded-result idea as ADR-001 Decision 3, so a caller on a Class C path can
observe and react to a refused audit write rather than have it silently disappear into a `catch (...)`.

### 3. Persistence, signing, and export are explicitly a separate, later ADR

This ADR defines the shape and the delivery contract of an `AuditEvent`. It does **not** define:
- where audit events are stored durably (file, flash, remote sink),
- how "tamper-proof... cryptographic signatures" (README) would be implemented (hash chaining,
  signing key management, and where that key material can live are all governed-zone-incompatible
  concerns — key handling is not allocation-free or side-effect-free by nature),
- the wire/export schema a "regulatory reporting: automated compliance report generation" feature
  would consume.

Each is a real, larger design question that deserves review on its own, the same way MduX split its
trust-zone ADR (004), its error-handling ADR (005), and its evidence-pipeline ADR (007) into three
documents rather than one. Bundling them here would either block this ADR on unresolved key-
management questions or force premature answers to them.

### 4. State the same scope limits MduX had to state explicitly, from the start

MduX's `docs/regulatory-compliance.md` had to be written specifically to correct its own README's
overclaiming (marking Risk Management System / Quality Management System / DHF / RMF as
"Completed" when no such code existed). mddlog's README already uses honest `(planned)` markers for
most of this area, which this ADR should preserve rather than erode: an `AuditEvent` type existing
does not mean mddlog operates an audit trail, provides tamper-evidence, or satisfies any clause of
IEC 62304/ISO 13485/ISO 14971 by itself. A manufacturer integrating mddlog remains responsible for
their own risk file, their own QMS, and their own regulatory engagement — mddlog supplies a record
shape and a delivery contract, not a certified audit subsystem.

## Alternatives Considered

### 1. Keep `AUDIT` as the top `LogLevel`, document the filtering behavior as-is (Rejected)
**Pros:** No new type; answers issue #5's open question with "no change."
**Cons:** Cannot express "this INFO-severity event must never be dropped" — severity and
audit-relevance are different questions, and collapsing them into one enum forces every audit-worthy
event to also be reported as maximally severe, which degrades the severity axis's own usefulness for
triage.

### 2. Depend on MduX's `mdux.governance` module directly (Rejected)
**Pros:** Reuses a reviewed, tested type instead of designing a new one.
**Cons:** `README.md` states "No dependencies except `import std`" as a Technical Requirement; taking
a dependency on another C++23-modules project (itself experimental, per MduX's own README warning)
would also make mddlog's compiler/CMake floor a function of MduX's, which is a much larger coupling
than an audit record needs. Where the *shape* is worth reusing (categories, single timestamp
spelling, sequence numbers), this ADR copies the pattern rather than the dependency, and could later
converge on a shared JSON schema between the two projects without a code dependency, if useful.

### 3. Cryptographic signing in this same ADR (Rejected — deferred to Decision 3)
**Pros:** One document covering the whole "regulatory audit" README claim at once.
**Cons:** Key management, signing algorithm choice, and storage medium are independent decisions
with their own alternatives and risks; forcing them into this ADR either blocks it on those
unresolved questions or answers them too quickly. MduX's own evidence pipeline (ADR-007) took the
opposite approach for a related problem — SHA-256 digests without commit-SHA self-reference — and
explicitly rejected embedding a stronger, harder-to-get-right guarantee in the same document as the
weaker one it could actually deliver (see ADR-007 Decision 5 in that project).

## Consequences

### Positive
- Gives issue #5's open "Audit policy" question a concrete, motivated answer instead of leaving it
  to be decided ad hoc when the corresponding tests are written.
- Separates "how severe" from "must not be lost", which the current single-enum design cannot
  express, without requiring the allocation-free work of ADR-001 to be finished first (the two
  ADRs are complementary, not sequential — `AuditEvent` can start with `std::string` fields and move
  to fixed-capacity ones when ADR-001 lands).
- Keeps the door open to a future MduX/mddlog shared audit-record *schema* (JSON-level
  interoperability) without taking on a compile-time dependency between the two projects.

### Negative
- A second delivery path (bypassing the ordinary sink pipeline) is more code to maintain than routing
  everything through one queue, and needs its own tests — which issue #5 does not currently scope,
  since it predates this ADR.
- Deferring persistence/signing means the README's "tamper-proof... cryptographic signatures" and
  "Automated compliance report generation" claims remain `(planned)` after this ADR, not delivered by
  it — this ADR only makes the eventual signing/export ADR easier to write by fixing what it would
  operate on.

### Risks and Mitigations
- **"Bypasses filtering" is read as "audit events cannot be disabled/tested", making them awkward in
  unit tests.** *Mitigation*: the delivery-outcome contract (Decision 2) still lets a test sink
  observe and count audit events normally; "bypasses `setMinLevel()`" is not the same as "cannot be
  redirected to an in-memory sink for testing", which issue #5's own "Targeted regression coverage"
  table already expects ("Audit policy: Define and test whether `logAudit()` intentionally
  bypasses...").
- **The category set (`Lifecycle, Configuration, Access, RiskControl, Operator`) is guessed, not
  requirements-driven.** *Mitigation*: treat it as a draft closed set to be revised before
  "Accepted", the same way MduX's `AuditCategory` (`Lifecycle, Verification, Change`) was scoped to
  that project's actual design-history use rather than invented speculatively — mddlog's set should
  be checked against a real device scenario before this ADR is accepted, not assumed correct here.

## References
- [MduX ADR-007: Evidence pipeline doctrine](https://github.com/ambroise-leclerc/MduX/blob/main/docs/adr/ADR-007-evidence-pipeline-doctrine.md) — precedent for splitting a weaker, deliverable guarantee from a stronger, deferred one across separate decisions (Decision 5 there; Decision 3 here).
- [MduX `mdux.governance::AuditEvent`/`ComplianceProgram`](https://github.com/ambroise-leclerc/MduX/blob/main/include/mdux/governance/Governance.cppm) — the design-history record this runtime record adapts, and the reason the two are not the same type.
- [MduX `medui::ActionTrace`](https://github.com/ambroise-leclerc/MduX/blob/main/include/mdux/medui/Input.cppm#L846) — precedent for a `sequence`-numbered runtime trace whose persistence is explicitly left to the host/caller.
- [MduX `docs/regulatory-compliance.md`](https://github.com/ambroise-leclerc/MduX/blob/main/docs/regulatory-compliance.md) — the scope-limits framing this ADR's Decision 4 follows.
- mddlog issue #5 — the "Audit policy" open question this ADR answers, and the persistence/crypto exclusion this ADR preserves.
- ADR-001 (this repository) — the allocation-free field shapes `AuditEvent` should adopt once available.

## Approval
- **Decision Date**: not yet approved — drafted for review.
- **Approved By**: pending (project maintainer).
- **Review Date**: before any work implementing persistent or cryptographically-sealed audit storage begins — that work needs its own ADR, and should not start from an unreviewed draft of what an audit event even is.
