# ADR-002: Regulatory audit-event model, separate from application logging

## Status
Proposed — drafted for maintainer review, not yet acted on.

All MduX references in this record are pinned to commit
[`d972d77`](https://github.com/ambroise-leclerc/MduX/tree/d972d77bc5cefdbe105ad7933ee61746fb5eb45b),
the baseline mddlog issue #5 verifies against.

## Context

`README.md` lists, under Medical Device Compliance: "Audit Trail: Tamper-proof logging with
cryptographic signatures", "Risk Management: Hazard tracking and mitigation logging per ISO 14971",
"Regulatory Reporting: Automated compliance report generation", and an `AuditSink` marked
`(planned)`. None of it exists in the current module set. What exists instead:

- `AUDIT` is one more value of the `LogLevel` enum (`include/mddlog/core/LogLevel.cppm:24`), ordered
  as the **highest** severity, above `FATAL`. Severity and audit-relevance are conflated: nothing
  distinguishes "unusually severe" from "must be retained for regulatory purposes regardless of
  severity."
- `SimpleLogger::logAudit()` (`include/mddlog/core/Logger.cppm:133-141`) builds an ordinary
  `LogRecord` with `level = LogLevel::AUDIT`, sets three free-text fields (`auditEventType`,
  `riskLevel`, `complianceStandard` — `LogRecord.cppm:39-42`, the last defaulted to the literal
  `"IEC_62304"` in `setAuditInfo()`), and calls `processLogRecord()` **directly**.
- **What that means for filtering, precisely** — an earlier draft of this ADR got this backwards and
  it matters, because the decision below rests on it. `logAudit()` does *not* call `shouldLog()`, and
  `processLogRecord()` (line 252) does not consult it either; only the other `log()` overloads do
  (line 98). So today `logAudit()` **already bypasses** `setEnabled(false)` and the logger's
  `minLevel_` threshold. Two filters still apply downstream: `writeToSinks()` (lines 269-280) checks
  `sink->shouldLog(record.level)` and `sink->isEnabled()` per sink. And because `AUDIT` is the
  maximum `LogLevel` value, no valid severity threshold could exclude it on severity alone anyway.
  The gap is therefore not "audit events are wrongly filtered" but "the existing bypass is
  incidental, unstated and untested, and sink-level filtering still silently applies."
- Beyond filtering, an audit record shares every best-effort property of ordinary logging: the same
  unbounded queue, and the same `catch (...)` that discards a sink failure without counting it.
- There is no schema, no requirement or hazard linkage, no correlation between a requested action
  and its outcome, no cryptographic sealing, and no export path distinct from `ConsoleSink`'s
  human-readable line format (`sinks/ConsoleSink.cppm:64-99`).
- mddlog's own tracked work already names this gap without closing it: issue #5's "Audit policy" row
  asks to "Define and test whether `logAudit()` intentionally bypasses ordinary logger disablement/
  filtering", and its Boundaries section excludes "persistent/cryptographically protected audit
  storage" from that issue's scope.

This ADR does not implement cryptographic sealing or persistent storage — those stay out of scope
here too, for the reason issue #5 gives: they are a distinct, larger concern (key management,
storage medium, tamper-evidence guarantees) deserving its own ADR once this one settles what an
audit event *is* and what its delivery contract promises.

## Medical Device Considerations

### IEC 62304 / ISO 13485 / ISO 14971 implications
- **Problem resolution and release** both depend on reconstructing "what happened, when, and was it
  verified" from a record — the question MduX's `mdux.governance::AuditEvent`/`ComplianceProgram`
  types answer for that project's *design history*, generalized here to runtime events a deployed
  device produces.
- **Risk control and post-market surveillance**: a hazard-relevant runtime event (an alarm silenced,
  an interlock overridden, a configuration changed) is exactly what a post-market process expects to
  be able to reconstruct. A record that a sink can drop without counting it cannot serve that
  purpose.
- **Fail-closed delivery, not fail-open severity.** "Important enough to always show" is a severity
  question; "must never be silently lost" is a delivery-guarantee question. An audit event needs the
  second, and does not need to be the numerically highest severity to need it: an `INFO`-severity
  "operator acknowledged alarm" event is as audit-relevant as a `FATAL` one. Today's ordering, with
  `AUDIT` above `FATAL`, cannot express that at all.

### Risk management considerations
- A fabricated, duplicated or mis-attributed audit event is its own hazard. A device that records
  "interlock overridden, operator confirmed" when no confirmation occurred, or records one
  confirmation twice, is worse than one that logged nothing. This is why Decision 4 separates the
  *request* for a critical action from its *confirmation* and its *execution result*, rather than
  emitting one event that implies all three.
- **The logger does not decide device behavior.** Nothing in this ADR lets mddlog inhibit an action
  or drive the device to a safe state. It reports outcomes; the application decides what an audit
  refusal means for the procedure in progress. A boolean from a logging call is not a risk-control
  decision, and must not be treated as one.

## Decision

### 1. `AuditEvent` is a distinct type, not a `LogLevel` value

Introduce an `AuditEvent` record separate from `LogRecord`, shaped after
[MduX's `mdux.governance::AuditEvent`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/include/mdux/governance/Governance.cppm)
but adapted for a runtime record rather than a design-history one:

```text
AuditEvent {
    category        AuditCategory   — see Decision 6 on why this set is provisional
    action          a stable event/action identifier (what happened), not free text
    phase           Requested | Confirmed | Executed | Failed   — see Decision 4
    target          what the action applies to (a control, a setting, a data object)
    actor           who or what initiated it, when known
    requirementRef  bounded, optional reference to a requirement this action discharges
    riskRef         bounded, optional reference to a hazard/risk control
    correlationId   ties Requested/Confirmed/Executed/Failed events of one action together
    sequence        monotonic, scoped per Decision 5
    time            raw host-supplied time value, serialized by the adapter (Decision 5)
    detail          bounded descriptive text (truncatable, unlike the identifier fields)
}
```

Two properties of MduX's type deliberately do not carry over, and one does. Its
`mdux.governance::AuditEvent` is a design-history record with three categories (Lifecycle,
Verification, Change), populated by a build-time compliance program, and it allocates
(`std::string` fields) — which is unremarkable there because it is not on a device's runtime path.
(For accuracy: `governed-throw` in MduX names a *scan profile* that checks for throws and does not
forbid allocation; it is not a "tier" that authorizes exceptions. The first draft of this ADR used
that phrase loosely.) What does carry over is the single-spelling timestamp discipline and the
monotonic `sequence`, the latter borrowed from MduX's runtime
[`medui::ActionTrace`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/include/mdux/medui/Input.cppm)
rather than from its design-history record, for the same reason MduX put it there.

Field kinds follow ADR-001 Decision 2: `action`, `target`, `actor`, `requirementRef`, `riskRef` and
`correlationId` are **identifier-kind** (over-long values are refused, never truncated); `detail` is
descriptive text (truncatable, with the truncation flagged).

**The migration discards no field, but it does constrain values — and the difference matters.** The
current inputs map as: `message` → `detail`; `eventType` → `action`; `userId` → `actor`; `deviceId`
→ `target` (or the stream identity of Decision 5, where the device is the emitter rather than the
object acted on); `riskLevel` → `riskRef`; `complianceStandard` → dropped from the record as a
per-event field, since a literal `"IEC_62304"` on every event carries no information — if a real
per-event standard reference is needed it belongs in `requirementRef`.

An earlier revision claimed "nothing may be lost" without qualification, which is not sustainable
against ADR-001 Decision 2: `logAudit()` today takes unconstrained `std::string_view` values, while
`action`, `actor` and `target` become identifier-kind fields that **refuse** an over-long value
rather than storing part of it. A caller passing a 4 KiB `eventType` gets a refused event, not a
truncated one — and calling that "lossless" would be exactly the kind of wording this record is
supposed to avoid. So the migration owes three things the implementing issue must supply:

- **A stated grammar and limit per identifier field** (permitted bytes and maximum length), chosen
  from what real call sites pass rather than guessed, and documented as a public constraint — a
  limit callers cannot see is a limit they will violate.
- **A refusal that names the field**, per ADR-001 Decision 3, so an over-long `eventType` is
  distinguishable from a full ring at the call site.
- **A documented fallback for free-text values that legitimately exceed a limit**: they belong in
  `detail`, which truncates and flags, not in an identifier field. A migration note should say so
  for `message`, which is the only current input with no length expectation at all.

The honest summary is therefore: no field is dropped, every value is either stored whole or refused
with a reason, and nothing is silently shortened except `detail`.

### 2. Audit capture bypasses ordinary logger filtering — stated, not incidental

Formalize the bypass that already exists (see Context): recording an audit event does not consult
the logger's `minLevel_` or `setEnabled(false)`. Severity and audit-relevance are different axes;
an audit event is not "more severe", it is *non-optional*.

Sink-level filtering is a **separate** question and is decided separately: an audit-carrying sink
may not silently drop an audit event via `shouldLog()`/`isEnabled()`. Either it accepts audit events
or it is not registered on the audit path at all; a sink that is disabled while audit events are
routed to it is a configuration error the host must be told about, not a silent filter.

Tests for this (issue #5's "Audit policy" row) must distinguish four cases that today's code
conflates: logger disabled, logger threshold, sink disabled, and sink failure.

### 3. Delivery is a three-level contract, and only the first level is promised today

The first draft claimed audit events are "never subject to best-effort delivery" and returned an
`accepted` result. That is not a contract — it names only the instant of admission and says nothing
about what happens afterwards. Replace it with three explicitly separated levels:

1. **Admitted** — the event was inserted into the bounded audit buffer (ADR-001 Decision 4). This is
   what the call's return value reports, and it is the only level this ADR promises.
2. **Handed off** — a consumer took the event and acknowledged it. Reported asynchronously, not by
   the producing call.
3. **Durably confirmed** — a storage backend confirmed it survives restart. **No backend offers this
   today**; the level exists in the contract so that a future persistence ADR has somewhere to
   attach it, and so that nothing in the meantime can be read as promising it.

Three consequences follow, and they are the substance of this decision:

- **The audit lane refuses rather than overwrites.** ADR-001 Decision 4 settles on refuse-new for
  exactly this reason: an event that was admitted and then silently overwritten to make room is
  worse than one that was refused up front, because its producer was told it succeeded.
- **Post-admission losses have a reporting channel.** Anything lost after admission — a consumer
  that fails, a sink that throws — is counted and surfaced through a dedicated audit-health signal,
  not through the producing call's return value and never through a bare `catch (...)`.
- **Power loss is explicitly outside every guarantee** while persistence is deferred. In-memory
  admission survives nothing.

**Worked scenario — ring full, then consumer unavailable.** (a) Steady state: `record()` returns
`Admitted`; the host observes a stable refusal counter. (b) The consumer stalls; the buffer fills;
subsequent `record()` calls return `Refused(RingFull)` and the refusal counter climbs — the host
sees refusals *at the call site*, immediately, and can decide (its decision, not the logger's) to
inhibit the operation, degrade, or continue. (c) The consumer then fails outright after having taken
events it never acknowledged: those events are not durable and not confirmed; the audit-health
signal reports unacknowledged-at-failure, and the events already admitted but undrained remain in
the buffer until a consumer returns or the device restarts. (d) Restart: everything in memory is
gone, and the new stream identity (Decision 5) makes the discontinuity visible rather than letting
the sequence appear continuous across the gap.

### 4. Request, confirmation and execution result are separate events

MduX's `ActionTrace{nodeId, requirement, event, sequence}` describes a critical action the host is
*about to* execute; MduX explicitly does not confirm that the device carried it out (`Input.cppm`'s
own comment: the host "owns the orderly-stop behavior, its timing and its audit persistence"). A
model that collapses that into one audit event would record an intention as if it were an outcome.

So a critical action produces up to four correlated events sharing one `correlationId`:
`Requested` (resolved from the UI/host input), `Confirmed` (operator acknowledgement, where the
workflow has one), `Executed` or `Failed` (reported by the host after acting).

**Concrete conversion from an MduX `ActionTrace`**:

| ActionTrace field | AuditEvent field |
|---|---|
| `nodeId` | `target` |
| `requirement` | `requirementRef` |
| `event` (`SystemEvent`) | `action` — the closed `SystemEvent` set maps to stable action identifiers; `category`/`phase` do **not** substitute for it |
| `sequence` | `sourceSequence` (provenance only) — **not** `AuditEvent.sequence` |
| — | `correlationId`, derived from the source's own stream identity and `sequence` |
| — | `sequence`, allocated fresh from the audit stream for this event and for every follow-up |
| — | `phase = Requested`; the host emits the matching `Executed`/`Failed` itself |

**The two counters are not the same unit, and an earlier revision wrongly equated them.** One
source action becomes several audit events (Decision 4), while `AuditEvent.sequence` must be
monotonic and unique per audit stream (Decision 5). Copying `ActionTrace.sequence` across breaks
both ways: source action 41 emits `Requested`, and if its `Executed` takes the next number 42, the
conversion of source action 42 then collides with it; reusing 41 for the result duplicates instead.
Adding `Confirmed`, or interleaving two actions, makes it worse — a counter that only counts source
actions reserves no room for the events each one produces.

So **every emitted event draws a fresh audit sequence number**, and the source counter is kept as
provenance. Correlation, not sequence, is what ties an action's events together — and
`correlationId` must be derived from `(source stream identity, source sequence)`, or allocated
independently, never from a bare `sequence` value: sequences restart per stream (Decision 5), so
two streams would otherwise mint the same id and a reader would group unrelated events.

**Worked example.** Audit stream `S`. Source action 41 (`emergency-halt`) produces `Requested`,
`Confirmed`, then `Executed` — audit sequences 100, 101, 102, all carrying
`correlationId = C(src, 41)` and `sourceSequence = 41`. Source action 42 follows with `Requested`
and `Failed` — audit sequences 103 and 104, `correlationId = C(src, 42)`. Now interleave: action 43
is requested (105, `C(src, 43)`), action 44 is requested (106, `C(src, 44)`), 44 executes (107,
`C(src, 44)`), 43 fails (108, `C(src, 43)`). Audit sequences are unique and increasing throughout;
each action's events remain joinable by `correlationId` regardless of interleaving; and no source
sequence value was ever used to order the audit stream.

These references stay generic and bounded: mddlog imports nothing from MduX, and no consumer is
required to populate `requirementRef`/`riskRef`. The conversion is documented here so the category
set can be validated against a real need rather than assumed (Decision 6).

### 5. Time and sequence: the host supplies time; the sequence is scoped and stream-identified

A canonical UTC rendering makes timestamps comparable; it does **not** establish emission order.
Civil time can step backwards, several events can share one instant, and a counter restarts after
reboot. A sequence number is also not cryptographic integrity — it orders, it does not attest.

- **The governed core stores a raw host-supplied time value**; ISO-8601 rendering happens in the
  adapter at serialization (consistent with ADR-001 Decision 1). When the adapter renders it, it
  uses one spelling only — fixed fractional-second width, UTC — so that two rendered timestamps
  order by string comparison.
- **A distinct monotonic value** may be carried for durations and ordering when civil time is
  unreliable; an explicit "civil time unavailable/unreliable" state is representable, rather than
  being encoded as a zero or an epoch value.
- **Sequence scope**: assigned by the producer, monotonic **per stream**, where a stream is one
  producer within one boot session. Its exhaustion behavior and width must be stated by the
  implementing issue (a 64-bit counter at any plausible event rate does not wrap within device
  lifetime, which is the intended answer, but it should be written down rather than assumed).
- **Stream identity must identify the producer, not only the boot.** An earlier revision described
  it as "a boot-session identifier that changes on restart", which distinguishes restarts but not
  the concurrent producers ADR-001 Decision 4 explicitly allows (one ring each, aggregated by the
  adapter). Two producers A and B in boot session `S` each emitting their event 1 would both carry
  `(S, 1)`, and after aggregation nothing says which stream an event came from: the
  `(stream, sequence)` ordering below becomes ambiguous, and so does the per-stream scoping ADR-004
  gives its chains. The identity is therefore unique **per stream instance** —
  `(deviceId, bootSessionId, producerInstanceId)`, or an opaque identifier with the same uniqueness
  property, where a globally unique identifier may leave some components implicit but must not give
  up the uniqueness. It is preserved through serialization, and **a producer destroyed and recreated
  within one boot session gets a new stream identity** whenever its counter restarts.
- No global order is promised across independent producers or across devices; `(streamId, sequence)`
  identifies an event unambiguously, and that is the whole promise.

**Worked example.** Two events are recorded in the same millisecond: identical rendered timestamps,
`sequence` 41 and 42 in one stream — order is unambiguous within the stream. The clock is then
corrected backwards by two seconds: event 43 renders with an *earlier* timestamp than 42, and
`sequence` is what tells a reader 43 came after; an analysis tool must therefore order by
(stream, sequence) and treat the timestamp as an attribute, not as the ordering key. The device
restarts: a new stream identity appears and `sequence` restarts, so 41/42 of the old stream and
41/42 of the new one are never confused, and the gap is visible instead of implied.

**Second worked example — two concurrent producers, then one of them restarts.** Producers A and B
run in boot session `S` on device `D`, each with its own ring. Both emit their first event, so both
hold `sequence = 1`; they are distinguishable only because their stream identities differ —
`(D, S, A)` and `(D, S, B)`. After the adapter aggregates both rings, every event still answers
"which stream" unambiguously, and a reader sorting by `(streamId, sequence)` gets two well-ordered
sequences rather than one interleaved guess. Producer A is then torn down and recreated inside the
same boot session; its counter restarts at 1, so it receives a **new** producer instance identity
`(D, S, A')` — otherwise its new event 1 would be indistinguishable from its old one. Nothing here
claims A's events and B's events have a defined relative order; only that no event is ambiguous.

### 6. Scope limits, stated from the start

The provisional category set — `Lifecycle`, `Configuration`, `Access`, `RiskControl`, `Operator` —
is **drafted, not requirements-derived**. It must be validated against the three scenarios the
maintainer's review asks for (critical action with a distinct execution result; saturation then
consumer failure; clock correction and restart) before this ADR moves to Accepted. MduX's own
`AuditCategory` was scoped to that project's actual use rather than invented speculatively.

And the limit that matters most: an `AuditEvent` type existing does not mean mddlog operates an
audit trail, provides tamper-evidence, or satisfies any clause of IEC 62304/ISO 13485/ISO 14971. A
manufacturer integrating mddlog remains responsible for their own risk file, their own QMS, and
their own regulatory engagement. mddlog supplies a record shape and a bounded delivery contract —
not a certified audit subsystem. MduX had to write
[`docs/regulatory-compliance.md`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/regulatory-compliance.md)
specifically to walk back its own README's "Completed" claims; mddlog's README uses honest
`(planned)` markers today, and this ADR must not erode them.

## Alternatives Considered

### 1. Keep `AUDIT` as the top `LogLevel` and document current behavior (Rejected)
**Pros:** No new type; answers issue #5's question with "no change".
**Cons:** Cannot express "this INFO-severity event must not be lost", cannot carry
requirement/hazard linkage or request-vs-result correlation, and leaves the existing bypass
incidental. It also leaves sink-level filtering silently applicable to audit events.

### 2. Depend on MduX's `mdux.governance` directly (Rejected)
**Pros:** Reuses a reviewed, tested type.
**Cons:** `README.md` requires "No dependencies except `import std`", and MduX's own README calls it
experimental; taking the dependency would also couple mddlog's compiler/CMake floor to MduX's. This
ADR copies the *shape* and documents a conversion (Decision 4) instead. A shared JSON schema between
the two projects remains possible later without a code dependency.

### 3. Cryptographic signing and persistence in this ADR (Rejected — deferred)
**Pros:** One document covering the whole README claim.
**Cons:** Key management, algorithm choice and storage medium are independent decisions with their
own risks; bundling them either blocks this ADR or answers them too quickly. Decision 3's third
level is the attachment point for that future ADR.

## Consequences

### Positive
- Gives issue #5's open "Audit policy" question a documented answer grounded in what the code
  actually does, and names the four test cases that answer needs.
- Separates severity from must-not-be-lost, and separates admission from delivery — neither of which
  the current single-enum, best-effort design can express.
- The ActionTrace conversion (Decision 4) gives the category set a real workload to be validated
  against instead of remaining a guess.

### Negative
- A second delivery path (bypassing the ordinary sink pipeline) is more code and more tests than one
  queue, and issue #5 does not currently scope them.
- The record grows several identifier fields, each with a capacity that must be chosen; too small
  refuses legitimate values (ADR-001 Decision 2), too large wastes fixed footprint.
- The README's "tamper-proof... cryptographic signatures" and "automated compliance report
  generation" remain `(planned)` after this ADR — it only fixes what a future signing/export ADR
  would operate on.

### Risks and Mitigations
- **"Bypasses filtering" is read as "cannot be tested or redirected".** *Mitigation*: bypassing
  `setMinLevel()` is not the same as being unobservable — an in-memory recording sink still sees
  every audit event, which is what issue #5's regression table needs.
- **The three-level delivery contract is read as three delivered guarantees.** *Mitigation*: only
  level 1 is promised; level 3 has no backend at all today, and Decision 3 says so in the same
  paragraph that introduces it.
- **The category set is wrong because it was guessed.** *Mitigation*: Decision 6 makes validation
  against three concrete scenarios a precondition for Accepted status, not a follow-up.
- **A host treats a refusal as a risk-control decision.** *Mitigation*: the Medical Device
  Considerations section states that the application owns that decision; the logger reports, it does
  not inhibit.

## References
All MduX links pinned to `d972d77bc5cefdbe105ad7933ee61746fb5eb45b`.
- [MduX `mdux.governance::AuditEvent`/`ComplianceProgram`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/include/mdux/governance/Governance.cppm) — the design-history record this runtime record adapts, and why the two differ.
- [MduX `medui::ActionTrace`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/include/mdux/medui/Input.cppm) — the request-only trace Decision 4 converts from, and its explicit "the host owns execution and audit persistence" boundary.
- [MduX ADR-007: Evidence pipeline doctrine](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/ADR-007-evidence-pipeline-doctrine.md) — precedent for splitting a deliverable guarantee from a deferred stronger one.
- [MduX `docs/regulatory-compliance.md`](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/regulatory-compliance.md) — the scope-limits framing Decision 6 follows.
- mddlog issue #5 — the "Audit policy" question this ADR answers, and the persistence/crypto exclusion it preserves.
- ADR-001 (this repository) — the bounded ring, the refuse-new overflow policy, and the field-kind truncation rules this ADR relies on.

## Approval
- **Decision Date**: not yet approved — drafted for review.
- **Approved By**: pending (project maintainer).
- **Review Date**: before any work on persistent or cryptographically-sealed audit storage begins, and after the three validation scenarios in Decision 6 are documented.
