# ADR-004 milestone A: acceptance review file

This file prepared the maintainer's decision. **Outcome (2026-10-03): the maintainer accepted
ADR-004**, recorded in the ADR's Approval section. Decisions 1 and 7 to 11 are accepted as written;
Decision 11's items stay deferred with no lot opened; milestone B (#89 to #93) is released from its
dependency on acceptance. The questions below are kept as the record of what was decided. Acceptance of the ADR is also distinct from implementation (#89 to
#93) and from validation evidence (#93). Nothing here is implemented.

## 1. Questions of Decision 4

| Question | Outcome | Where |
|---|---|---|
| Anchoring mechanism (blocking) | Resolved | Decision 7 (#85) |
| Canonical byte contract (blocking) | Resolved | Decision 8 (#84) |
| Storage medium and layout | Resolved | Decision 9 (#86) |
| Power-loss atomicity, "durably confirmed" | Resolved | Decision 9 (#86) |
| Retention and rotation | Resolved | Decision 10 (#87) |
| Chain state recovery | Resolved | Decision 10 (#87) |
| Export format | Deferred, with reason | Decision 11 |
| Signing, key custody, provisioning, rotation | Deferred, with reason | Decisions 3 and 11 |

No question is open by silence.

## 2. Claims

- README, ADRs, documentation and source comments were searched for tamper-proof, tamper-resistant,
  immutable, unforgeable, signature, compliance-report and authorship claims. Remaining occurrences
  are historical quotations of the earlier README wording, or statements of what is not provided.
- ADR-002's consequence on the former README claims and the ADR index were updated to match.
- Allowed and forbidden wording is fixed in ADR-004 Decision 1.

## 3. What the maintainer is asked to decide

1. Accept, amend or reject Decisions 1 and 7 to 11 as the design of record.
2. Confirm the deferred items of Decision 11 and whether each gets a lot under epic #11 now. This file
   opens none.
3. Release milestone B (#89 to #93) from its dependency on acceptance, or hold it.

## 4. Known limits to weigh

- The exposure window past the last anchor is undetectable by construction (7.5).
- Detecting a joint rollback needs state kept off the device (7.4).
- The initial backend offers no authorship evidence and no reporting.
- Decision 8's reference vectors were reproduced by two independent implementations, which the
  record states; repeating that on the project's toolchains belongs to #89 and #93.
