# Architecture Decision Records

This index lists mddlog's ADRs. Format follows the sibling project
[MduX's ADR convention](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/docs/adr/README.md):
Status, Context, Medical Device Considerations, Decision, Alternatives Considered, Consequences,
References, Approval. MduX links are pinned to commit `d972d77bc5cefdbe105ad7933ee61746fb5eb45b` —
the baseline issue #5 verifies against — rather than to a branch, so the comparisons stay checkable
as MduX moves.

An ADR record is **not** a claim that the described behavior is implemented. Distinguish a
"Proposed" ADR (a decision drafted for maintainer review, not yet acted on) from an "Accepted" one
(a decision the codebase is expected to already satisfy, is actively being brought into
conformance with, or has accepted as a design while implementation remains pending) the same way `AGENTS.md` asks the rest of this repository to: do not describe a
capability as delivered from an ADR's existence alone.

| ADR | Title | Status |
|---|---|---|
| [001](ADR-001-allocation-free-governed-logging-core.md) | Allocation-free governed logging core for IEC 62304 Class C | Accepted |
| [002](ADR-002-regulatory-audit-event-model.md) | Regulatory audit-event model, separate from application logging | Accepted |
| [003](ADR-003-application-integration-and-sink-ownership.md) | Application integration and sink ownership | Accepted |
| [004](ADR-004-audit-persistence-and-tamper-evidence.md) | Audit persistence and tamper evidence | Accepted |
| [005](ADR-005-contextual-logging-api.md) | Explicit contexts and producer bindings (#113/#127) | Accepted |
| [006](ADR-006-audit-tools-and-export.md) | Audit CLI and versioned projection/evidence (#119/#121) | Proposed for review |

## How the five relate

ADR-001 defines the bounded, allocation-free core and the zone boundary. ADR-002 builds the audit
record and its delivery contract on top of it, and promises in-memory admission only. ADR-003 goes
the other direction — what an existing application needs before it can adopt any of this, with
WebFront as the concrete consumer — and ADR-004 holds the storage and tamper-evidence questions
ADR-002 deferred. It is accepted as a design (milestone A, 2026-10-03); its implementation (#89 to #92) and the
evidence of #93 ([validation report](../audit-persistence-validation.md)) await a separate review: it keeps the claims
honest, and Decision 11 lists what it defers.

ADR-005 is accepted as the 1.0 usage-layer design, without amending the four preceding
contracts. Its contexts and producer bindings are implemented under `include/mddlog/`;
the [executable study](../../examples/ContextualUsage.cpp) now uses those public modules.
The [study report](../contextual-api-study.md) preserves the accepted prototype revision,
while [primitive verification](../contextual-api-validation.md) and
[integration review](../contextual-api-integration.md) document the implementation and
its evidence. The integration report records the maintainer closure decision and its
local-application reservation; this acceptance remains separate from design acceptance.

Further records are anticipated and deliberately not drafted: signing with key management
(custody, provisioning, rotation), **compliance reports**, which ADR-004
Decision 11 records as deferred work with the reason for each. The root `README.md` presents none of
them as a delivered feature. ADR-006 now proposes the export format around implemented canonical/layout/witness contracts.
Its optional CLI and codecs are a candidate implementation with local evidence, awaiting
maintainer schema review; this does not amend the preceding four contracts.

## Relationship to MduX

mddlog is a standalone library: its CMake targets do not link MduX, and its modules do not
import `mdux.governance` or `mdux.evidence`. Where these ADRs reuse a pattern MduX already
worked through (trust-zone separation, no-heap verification, structured audit records, canonical
serialization), they say so, state what the borrowed mechanism actually
checks, and adapt it to a type mddlog owns rather than importing MduX's. MduX describes itself as
experimental: its ADRs and targeted checks are an architectural precedent, not validation
transferable to mddlog.
