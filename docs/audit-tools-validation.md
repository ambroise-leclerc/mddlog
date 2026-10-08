# Audit tools verification (#119)

Candidate on branch `119-audit-tools`, based on `9b8b330` (develop). Results refer to
this working revision; final reviewed SHA/CI and schema acceptance remain to record.
No external Orin image, physical power-cut campaign or final #122 adoption is implied.

## Coverage

`audit-export-roundtrip` (VER-066) compares the captured/replayed library report with
the original LogVerifier report: every stream verdict/cause, range, position, anchor,
retained outcome, age, ledger disposition, boundary note and residual region. It checks
candidate retained heads/anchors, deterministic byte roundtrip, negative verification
time and age bounds. The medium refuses mutations.

`audit-export-refusals` (VER-067) rejects unknown package version, truncated inputs,
checksum corruption, trailing bytes, duplicates, oversized profiles and projections
presented as evidence. `audit-export-trust-budgets` (VER-068) checks unavailable anchors
without accepted trust, unchanged candidates after work refusal and adverse results
for changed canonical bytes.

`audit.tools` (VER-069) creates a real file-backed closed journal with two producer
streams and a ledger, durable file witness state and a read-only Unix witness service.
The CLI's captured/snapshot report matches a direct LogVerifier run on the real files.
It decodes JSON in a separate Python process and tests escaped byte strings, selection
metadata, full-report retention, new private files, refusal to overwrite, named reusable
configuration, reader enrollment/load/save and inspection without saving. It exercises
unavailable transport, explicit embedded trust, independent consultation, truncation,
unknown package/configuration versions, read/output/provider/work budgets and alteration
with a recomputed frame CRC but unchanged chain digest. The journal bytes remain unchanged
by inspection/export. Original v0.2 producer/layout bytes are supplied by
[the immutable-version corpus](../tests/archives/audit-export/README.md), not generated
by the current codec. Three producer events and a closed ledger keep anchored results;
a legacy directory without allocator metadata remains inspectable.

The CLI integration fixture uses one host UID to exercise protocol/report equivalence;
it does **not** qualify separate-authority deployment. The existing `witness.deployment`
proof tests four separate UIDs; deployment independence is governed by #115 and the host
profile. Embedded snapshot trust in the corpus is an explicit test assumption, never
an authentic externally signed witness. Final #120/#121 archive matrix remains open.

## Commands and local results

Reference profile: Linux x86_64, Clang/libc++ 21.1.8, CMake 4.2.3, Ninja, Release,
file adapters and optional CLI enabled. Use:

```sh
cmake --preset ninja-clang -DMDDLOG_BUILD_AUDIT_TOOLS=ON
cmake --build --preset ninja-clang --parallel 4
ctest --test-dir build-clang --parallel 4 --output-on-failure
scripts/check-format.sh
JOBS=8 scripts/run-clang-tidy.sh build-clang
python3 scripts/check-development-file.py
python3 -m unittest discover -s tests/documentation -p 'Test*.py'
```

Local results, 2026-10-08:

- Full Release build completed with the optional CLI enabled.
- Full CTest run: 234/235 passed initially; `witness.deployment` could not traverse the
  private repository path in its multi-UID namespace. Copying the same worker/service
  executables to `/tmp/mddlog-119-witness` and rerunning the unchanged deployment script
  passed its separate-UID authority/custody/restart/rollback checks. This is an explicit
  environment workaround, not a passing claim for the original CTest invocation.
- After adding age/unknown-layout cases, a test incorrectly assumed the persistent
  witness omitted accepted time. It was corrected to check actual Unknown/Stale semantics;
  all four export/CLI tests then passed, including the installed CLI. The remaining
  233 CTest checks passed in that run. No library age behavior was changed.
- Development-file checker passed; 59 documentation tests passed; format check passed
  on all 117 files with clang-format 21.1.8; `git diff --check` passed.
- No-file-adapter library configured **and built** in `/tmp/mddlog-119-portable`.
  Enabling the CLI with that backend disabled was rejected with the named CMake diagnostic.
- CLI installed into `/tmp/mddlog-119-install`; the complete external-process CLI campaign
  passed using the installed executable.

Static analysis completed with LLVM 21.1.8: the reference entry point analysed 101
translation units. 99 passed in that invocation; two units encountered a captured-budget
DeadStores diagnostic or a source/BMI mismatch during this editing session. The accumulator
is consumed by subsequent charges and reserve; a narrowly justified suppression is attached
to that reference-capture update. After reconstructing the final modules, all four changed
units (Evidence, Projection, export specs, CLI) passed `clang-tidy-21` with
`--warnings-as-errors='*'`, exit 0 and no project diagnostics. Thus the whole 101-unit scope
is covered across the global invocation and final rechecks; the earlier failed invocation
is not described as passing. No global disabling of checks is used. Generated system-header
warning counts are filtered by the project's existing analysis policy.

The final build, four export/CLI CTests and installed CLI campaign all passed after the
source-directory output refusal was added. Configuration success alone
is never a build or verification result. Source/install governed consumers retain their
separate checks. CLI installation and a no-file-adapter library build are checked as
separate optional configurations; Linux tools refuse configuration without that backend.

## Ergonomics review and limits

Common paths are one named command and reusable central configuration. The first line
reports trust, retained provenance, exit category and stream count; each stream states
present/anchored/unanchored positions, cause, age and rollback caveat. Failures identify
source permissions/lock, witness identity/ACL/deadline, configuration key, input budget,
checkpoint restoration or output destination. Export uses a new private file, and a
filtered projection remains visibly partial with a full report. A human report alone
cannot promise replay. Numeric causes/IDs are documented in the schema; no aggregate
validity flag or regulatory attestation is produced.

This is a technical review against the issue's workflows, not an independent user study
or maintainer acceptance. Very large host-defined profiles can require substantial memory:
multiple bounded buffers coexist and no RSS/WCET claim is made. Persistent storage and
checkpoint durability remain host-qualified. The optional Linux CLI is exercised on this
profile; the portable modules still need the release matrix's CI. GAP-012 stays open for
review/adoption; GAP-013/#120, GAP-014/#121 and GAP-015/#122 keep their own closure criteria.
