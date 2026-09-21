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

## Planned, not yet drafted

**ADR-003 — Application integration and sink ownership.** ADR-001 and ADR-002 define a bounded core
and an audit contract; neither is sufficient to replace an existing application logger. A separate
record is needed, covering at least:

- a facade over the existing API surface, and migration of the behaviors an existing consumer's test
  suite already pins;
- consumption without C++23 modules as an explicit decision (isolated adapter, header-compatible
  interface, or optional dependency) rather than an implicit promise — mddlog requires CMake 4,
  C++23 modules and `import std`, which a header-only consumer on an older CMake does not get for
  free merely because both projects use C++23;
- sink ownership and removal during concurrent emission, so a callback bound to a destroyed
  connection is never invoked;
- a transport-backed consumer (e.g. WebSocket): saturation, disconnection, and transport-error
  handling that cannot loop, where failing to send a log generates further logs;
- where formatting happens (adapter) versus where context is captured (producer — ADR-001
  Decision 1);
- an optional audit lane, kept distinct from diagnostics streamed to a UI or browser.

A persistence-and-tamper-evidence ADR is also deferred; ADR-002 Decision 3 names the attachment
point for it.

## Relationship to MduX

mddlog is a standalone library — `README.md` states "No dependencies except `import std`" — so it
cannot depend on MduX's `mdux.governance` or `mdux.evidence` modules directly. Where these ADRs
reuse a pattern MduX already worked through (trust-zone separation, no-heap verification, structured
audit records), they say so, state what the borrowed mechanism actually checks, and adapt it to a
type mddlog owns rather than importing MduX's. MduX describes itself as experimental: its ADRs and
targeted checks are an architectural precedent, not validation transferable to mddlog.
