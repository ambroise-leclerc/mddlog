# ADR-006 — Audit tools and versioned export (#119 / #121 milestone A)

## Status

Proposed for maintainer review, 2026-10-08. The implementation in this branch is a
reviewable candidate; neither schema freeze nor acceptance of #119/#121 is implied.

## Context

ADR-004 decision 11 deferred export. Canonical events, segment layout, witness
snapshots and reader checkpoints now exist. A reader needs inspection and portable
preservation without writing an application. The four preceding contracts remain intact.

## Medical Device Considerations

A technical integrity report is not a regulatory compliance attestation. A hash in
a mutable package detects accidental damage but authenticates neither writer nor witness.
Export may disclose actors, targets and detail; the caller chooses a private destination.

## Decision

Deliver an optional Linux executable `mddlog-audit` linked only to the adapter target.
It accepts named options or a strict versioned `key=value` configuration. `inspect`
produces a short human report or a JSON projection. `export` writes either a JSON
projection or a binary evidence package. `verify` reads a package without restoring
its files onto a live journal. `checkpoint-init` explicitly enrolls a reader store;
`--update-retained` explicitly saves a verified candidate with generation checking.
Inspection always uses a copy of retained state. Missing/damaged checkpoints are errors.

Two independently versioned formats start at 1: projection `mddlog.audit.projection`
and evidence `MDDAUDIT`. A projection contains events and full per-stream reports,
coverage, boundaries and residual regions; filtering selects events only, labels the
projection partial, and never changes the full verification report. It cannot be
imported as evidence. A package contains every segment byte including residual bytes,
a frozen witness listing or its unavailability, source and witness provenance, the
initial reader position, verification time/age bound and resource profile. It cannot
be filtered. A frozen provider is used for both export and replay so each run sees
one coherent listing. The report is recomputed from the package using library APIs;
a stored human report is unnecessary and never authoritative.

On replay, embedded anchors are unavailable by default. The user either supplies an
independent authenticated Unix witness, or explicitly accepts the embedded snapshot
with `--accept-embedded-provider`. The latter reports `accepted-embedded-assumption`:
it is an assumption, not proof of independence. The embedded retained position also
has no independent authenticity; an external reader checkpoint overrides it. Without
an independently retained checkpoint joint rollback remains outside the guarantee.

Unknown package versions, truncation, checksum errors, duplicate references, trailing
bytes and resource excess are refused before verification. Unknown canonical/layout
versions retain the library's CannotVerify meaning. Local input budgets bound decoding;
package budgets cannot enlarge them. No signature/key custody is added (#123).

Exit status priority: 2 operation impossible (configuration, I/O, budgets, unsupported
format); 3 inconsistency/alteration/rollback/conflict/incomplete history or adverse
ledger boundaries; 4 absent/partial coverage, unavailable witness, stale anchor or
unclosed history; 0 only complete anchored/retired coverage without those reservations.
Full per-stream findings remain visible; there is no aggregate `valid` field.

## Alternatives Considered

JSON as evidence duplicates and expands segment bytes and requires a dependency to
parse safely. Signed packages require #123. A projection-only tool fails replay needs.

## Consequences

The evidence codec and projection writer are portable adapter modules; only the CLI
requires the optional Linux adapters. No new dependency reaches `mddlog::core`.
Format field order, enum IDs and exit codes are governed separately from C++ API/BMI
compatibility; see [the wire contract](../audit-export-format.md). v0.2 journal layout
and canonical contracts are unchanged; permanent old-version archives and final
support duration remain governed with #120/#121. Source installation and CLI installation
are separate. The schema is a candidate until maintainer review.

## References

[ADR-004](ADR-004-audit-persistence-and-tamper-evidence.md),
[CLI usage](../audit-tools.md), [verification](../audit-tools-validation.md).

## Approval

No maintainer acceptance recorded for this candidate. Delivery, local verification,
schema acceptance and system qualification are distinct.
