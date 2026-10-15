# ADR-007 — Compatibility, formats and distribution commitments (#121 milestones A/B)

## Status

Proposed for maintainer review, 2026-10-09. It states the commitments mddlog intends to make for
1.0 and the evidence that already supports them. The final freeze waits for the independent
application feedback and acceptance of #122; until then every tier below is a **candidate** and
no 1.0 promise is in force. ADR-001 to ADR-006 are not amended.

## Context

mddlog is consumed as C++23 named modules, from a source tree or an installed CMake package. Up
to v0.3.0 the package declared `AnyNewerVersion` compatibility, mutated the consumer's directory
state from `mddlogConfig.cmake`, exported the project's warning target (so the consumer's rebuild
of mddlog's BMIs ran under `-Werror`) and leaked MSVC warning levels and macros into consumer
translation units. Nothing listed which modules and names were promised, how long old journals
remain readable, or which toolchains were qualified rather than merely admitted. ADR-006 versions
the export formats but defers their support duration here.

## Medical Device Considerations

An integrator records the exact configuration of the software it ships (IEC 62304 configuration
management, SOUP identification). A compatibility statement is only useful if it names what is
covered, what is not, and the evidence for each claim; an audit journal must keep its meaning
across library upgrades for as long as the device must read it. Neither a promise of ABI/BMI
portability between compilers nor a claim on unqualified toolchains would be honest: C++ modules
are rebuilt by each consumer with its own compiler and options.

## Decision

### 1. Surface tiers, recorded in an inventory

[`docs/api/public-surface.json`](../api/public-surface.json) lists every module, umbrella name,
installed target, package component and format version, each with a tier and the release that
introduced it. [`scripts/check-compatibility.py`](../../scripts/check-compatibility.py) fails
when the repository and the inventory disagree, and with `--git` when a name or module released
in a tag disappears without a `removed[]` record.

| Tier | Meaning once frozen |
| --- | --- |
| stable-candidate | `import mddlog;`, `import mddlog.log;`, the governed `mddlog.core.*` modules, the targets `mddlog::core` and `mddlog::mddlog`: source and behavior compatibility within a major version |
| extension | Supported building blocks (audit store/service/drain/verifier, contextual and text adapters, transport, Linux file adapters, the CLI): same promise; adding a pure virtual member to an interface an integrator implements needs a major version |
| format-codec | C++ codecs of versioned byte formats: the format is governed by its own version (section 3); the C++ API may change in a minor release with a changelog entry |
| implementation-module | Import name not promised; its symbols are supported only through the umbrella names |
| reduction-candidate | Low-level helpers still exported by the umbrella (encoders, layout scanners, colour codes): kept now, to be reviewed for deprecation before the freeze |
| test-double | `InMemoryStorageMedium`, `InMemoryAnchorProvider`: for tests and evaluation, never a deployment backend |
| link-only | `mddlog::mddlog_options`: installed because an exported target links it |

The SpecLab doubles of `tests/framework/` are not installed and are not part of any tier.
Duplicates identified with #113 (`logMedical` next to contexts, the global `Log` facade next to
injected bindings, low-level exports) stay available; any reduction is decided after #122 and
follows section 2.

### 2. Source and behavior compatibility, deprecation, versioning

- Versions follow SemVer. Before 1.0 a **minor** version may break source compatibility; the
  package therefore declares `SameMinorVersion` while the major is 0 and `SameMajorVersion`
  from 1.0 (`CMakeLists.txt`).
- Within a major version after the freeze: no removal or signature change of a stable-candidate
  or extension name; additions only. Behavior changes are limited to bug fixes and to outcomes
  already allowed by the documented contract (for example a refusal the API already reports).
  A changed meaning of a result, a verdict or stored bytes requires a major version.
- Deprecation: `[[deprecated("...")]]` plus a changelog entry and a migration note in a minor
  release, at least one minor release before removal in the next major. Removal is recorded in
  `removed[]` with the deprecation and removal versions and its migration guide.
- **No ABI or BMI promise.** Objects, archives and BMIs are compatible only with the exact
  compiler, standard library and ABI-affecting options they were built with. Consumers rebuild
  BMIs from the installed interface units; a different compiler or version must rebuild mddlog.
  The package refuses a compiler id/version different from its build unless the integrator sets
  `MDDLOG_ACCEPT_UNQUALIFIED_TOOLCHAIN`, which only downgrades the refusal to a warning.

### 3. Persistent and exchanged formats

Formats are versioned independently of the C++ API: canonical event, segment layout, anchor
claim, witness protocol, retained position, evidence package, projection JSON, CLI configuration,
build-info and source manifests (inventory section `formats`). Rules:

- One version, one meaning: any change to the meaning of bytes, fields, identifiers or verdicts
  increments the format version. The checker fails when a version constant changes without the
  inventory, so a format change cannot be silent in a review.
- Readers refuse unknown versions explicitly: the library reports `CannotVerify`/unsupported
  layout, the CLI exits with status 2 (ADR-006). No best-effort reinterpretation.
- **Read duration**: every format version written by a published release from v0.2.0 on stays
  readable and verifiable by every later release of the same major version, and by the first
  release of the next major. Dropping read support needs a major version, announced one minor
  release earlier, with an export or conversion path published before the removal.
- Writers emit only the current version of each format.
- Every published release that produces audit journals adds an immutable archive fixture with
  its generator and provenance under [`tests/archives/audit-export`](../../tests/archives/audit-export/README.md);
  `check-compatibility.py --git` fails when a release tag from v0.2.0 on has none. v0.2.0 and
  v0.3.0 are present. Fixtures are never regenerated with a newer library.

### 4. Distribution: targets, components and consumer requirements

The installed package provides `mddlog::core` (governed modules only), `mddlog::mddlog` and, when
built, the Linux file adapters and `mddlog::audit_tool`, selectable as components `core`, `full`,
`file_storage` and `audit_tool`. Optional components never add a dependency to `mddlog::core`
(ADR-001 decision 6, ADR-006). The package states its build in `mddlog-build-info.json` (source
revision, toolchain, options, components, link dependencies) and changes no consumer variable,
directory option or standard. Project warnings, MSVC exception/conformance options and version
macros are private to mddlog's own targets; CMake still rebuilds installed BMIs with the
definitions and options their interface units were compiled with. The consumer requirements and
the BMI-rebuild constraints are in [the consumer requirements](../consumer-requirements.md).

### 5. Matrix and reference profile

The **qualified** matrix is the set of exact tuples a release's CI exercises with build, test and
consumer runs, listed in [the compatibility matrix](../compatibility-matrix.md); the admission
floors of `CMakeLists.txt` are wider and are not a qualification. The reference profile for the
full feature set (file adapters, CLI, sanitizers, fuzzing) is Linux x86_64 with upstream Clang 21
and libc++. Debug and Release are both exercised; an installed Release library serves a Debug
consumer on GCC/Clang, while MSVC consumers keep the library's configuration. Widening a bound
follows the qualification procedure of the matrix document; a refused combination fails at
configure time with a message naming the supported alternative.

### 6. Reproducible source distribution

A source release is produced by [`scripts/package-source.py`](../../scripts/package-source.py)
from a commit: tracked files only, sorted, normalized metadata, commit timestamp, plus
`SOURCE_REVISION` and a manifest of file digests and pinned dependencies. The tar stream is
reproducible from the commit; the gzip bytes also depend on zlib, so both digests are recorded.
No package-manager integration is chosen: none has demonstrated support for installed C++ modules
with `import std` on this matrix, and adopting one waits for that evidence.

## Alternatives Considered

- Keeping `AnyNewerVersion`: it accepts 0.x minors that may break source compatibility.
- Promising ABI/BMI portability or prebuilt BMIs: not achievable with today's compilers and not
  verifiable by this project.
- Letting the package configure the consumer (standard, `import std`, flags): it cannot set the
  `import std` gate in time anyway, and it silently changed unrelated targets of the consumer.
- Freezing now: #122 has not yet fed back; freezing the candidate tiers would preempt that review.

## Consequences

Consumers relying on the removed package side effects must set C++23 and `import std` themselves
and request the minor version they use ([migration](../migration/v0.2-to-1.0.md)). Four CTest
entries build and run the independent consumer of `tests/consumer/package` against the installed
full and core packages, a source subdirectory and an extracted source archive, with the
consumer's own strict warnings. The inventory and its checker run in the documentation CI. Open
before 1.0: tier decisions on reduction candidates, the pinned patch versions of the Linux Clang
and Windows MSVC CI legs, the #147 TSan/libc++ observation, and acceptance by #122.

## References

[ADR-001](ADR-001-allocation-free-governed-logging-core.md), [ADR-004](ADR-004-audit-persistence-and-tamper-evidence.md),
[ADR-006](ADR-006-audit-tools-and-export.md), [export formats](../audit-export-format.md),
[validation of this lot](../compatibility-validation.md), [release process](../release-process.md).

## Approval

No maintainer acceptance recorded. Delivery of this lot, its local evidence, acceptance of the
commitments and the 1.0 freeze are distinct decisions.
