# Architecture Decision Records

This index lists mddlog's ADRs. Format follows the sibling project
[MduX's ADR convention](https://github.com/ambroise-leclerc/MduX/blob/main/docs/adr/README.md):
Status, Context, Medical Device Considerations, Decision, Alternatives Considered, Consequences,
References, Approval.

An ADR record is **not** a claim that the described behavior is implemented. Distinguish a
"Proposed" ADR (a decision drafted for maintainer review, not yet acted on) from an "Accepted" one
(a decision the codebase is expected to already satisfy or is actively being brought into
conformance with) the same way `AGENTS.md` asks the rest of this repository to: do not describe a
capability as delivered from an ADR's existence alone.

| ADR | Title | Status |
|---|---|---|
| [001](ADR-001-allocation-free-governed-logging-core.md) | Allocation-free governed logging core for IEC 62304 Class C | Proposed |
| [002](ADR-002-regulatory-audit-event-model.md) | Regulatory audit-event model, separate from application logging | Proposed |

## Relationship to MduX

mddlog is a standalone library — `README.md` states "No dependencies except `import std`" — so it
cannot depend on MduX's `mdux.governance` or `mdux.evidence` modules directly. Where these ADRs
reuse a pattern MduX already validated (trust-zone separation, no-heap verification, structured
audit records), they say so and adapt the pattern to a type mddlog owns, rather than importing
MduX's types. See each ADR's References section for the specific precedent.
