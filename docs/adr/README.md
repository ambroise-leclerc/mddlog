# Architecture Decision Records

This index lists mddlog's ADRs. Format follows the sibling project
[MduX's ADR convention](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/README.md):
Status, Context, Medical Device Considerations, Decision, Alternatives Considered, Consequences,
References, Approval. MduX links are pinned to commit `d972d77bc5cefdbe105ad7933ee61746fb5eb45b` —
the baseline issue #5 verifies against — rather than to a branch, so the comparisons stay checkable
as MduX moves.

An ADR record is **not** a claim that the described behavior is implemented. Distinguish a
"Proposed" ADR (a decision drafted for maintainer review, not yet acted on) from an "Accepted" one
(a decision the codebase is expected to already satisfy or is actively being brought into
conformance with) the same way `AGENTS.md` asks the rest of this repository to: do not describe a
capability as delivered from an ADR's existence alone.

| ADR | Title | Status |
|---|---|---|
| [001](ADR-001-allocation-free-governed-logging-core.md) | Allocation-free governed logging core for IEC 62304 Class C | Proposed |
| [002](ADR-002-regulatory-audit-event-model.md) | Regulatory audit-event model, separate from application logging | Proposed |
| [003](ADR-003-application-integration-and-sink-ownership.md) | Application integration and sink ownership | Proposed |
| [004](ADR-004-audit-persistence-and-tamper-evidence.md) | Audit persistence and tamper evidence | Proposed |

## How the four relate

ADR-001 defines the bounded, allocation-free core and the zone boundary. ADR-002 builds the audit
record and its delivery contract on top of it, and promises in-memory admission only. ADR-003 goes
the other direction — what an existing application needs before it can adopt any of this, with
WebFront as the concrete consumer — and ADR-004 holds the storage and tamper-evidence questions
ADR-002 deferred, in a state of "bounded, not settled": it exists to keep the claims honest while
its own Decision 4 lists five questions it does not answer.

One further record is anticipated and deliberately not drafted: an **export format** for the
"automated compliance report generation" claim in the root `README.md`, which ADR-004 Decision 4
lists among its open questions. It is left out rather than stubbed, because an export format is read
by tools this project does not control and should not be sketched before there is something to
export.

## Relationship to MduX

mddlog is a standalone library — `README.md` states "No dependencies except `import std`" — so it
cannot depend on MduX's `mdux.governance` or `mdux.evidence` modules directly. Where these ADRs
reuse a pattern MduX already worked through (trust-zone separation, no-heap verification, structured
audit records, canonical serialization), they say so, state what the borrowed mechanism actually
checks, and adapt it to a type mddlog owns rather than importing MduX's. MduX describes itself as
experimental: its ADRs and targeted checks are an architectural precedent, not validation
transferable to mddlog.
