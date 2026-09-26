# Changelog

Written for a reader deciding whether a version does what they need, and for an auditor asking what
changed between two artefacts they hold. Entries name the issue that owns the work, because the
issue carries the reasoning and this file carries the outcome. The release procedure, including when
an entry moves from `unreleased` to a date, is [`docs/release-process.md`](docs/release-process.md).

mddlog is an experiment shaped by IEC 62304, ISO 13485 and ISO 14971 concerns. No entry in this file
is a certification, a validation for medical-device use, or a production-readiness claim.

---

## 0.1.0 — unreleased

The first version, closing epic #8: a bounded, allocation-free governed logging core separated from
the allocating adapter and sinks, as accepted in [ADR-001](docs/adr/ADR-001-allocation-free-governed-logging-core.md).
It also carries the build, verification and convention work that preceded it (#5, #7).

### The governed core, `mddlog::core` (epic #8)

- **Contracts first (#31, realigned by #46).** ADR-001 fixes capacities in bytes, the memory budget,
  host-supplied time, per-field truncation rules and the admission result before any code, and was
  accepted on 2026-09-24. #46 later aligned its names and history with the implementation.
- **Two CMake targets with a one-way dependency (#32).** `mddlog::core` holds only governed modules;
  `mddlog::mddlog` links it and adds the adapter and sinks. `SimpleLogger` and the allocating
  `LogRecord` moved to `mddlog.adapter.logger` and `mddlog.adapter.logrecord`, with no compatibility
  shim under `mddlog.core.*`.
- **`InlineString<N>` (#33)**: owned, fixed-capacity, `constexpr` and `noexcept`. Identifiers are
  stored whole or refused; descriptive text is truncated without splitting a UTF-8 sequence.
- **`WriteResult` (#34)**: admission, the first refusal reason (`RingFull`, `IdentifierTooLong` with
  the offending field, `MalformedTime`) and message truncation, returned by value with an invariant
  its accessors cannot break.
- **`GovernedRecord` (#35)**: a bounded record capturing level, host-supplied `RawTime`, source
  location, message and the component/operation/correlation identifiers at emission. A refused
  assignment leaves the previous record unchanged; the core reads no clock.
- **`RingLog<Capacity>` (#36)**: a single-producer/single-consumer ring with inline storage that
  refuses new writes when full rather than overwriting, uses the two release/acquire exchanges of
  ADR-001 Decision 4, counts `RingFull` refusals atomically, and drains through one or two read-only
  spans until an explicit, validated acknowledgement.
- **Concurrency under ThreadSanitizer (#37)**: producer/consumer scenarios on separate threads, with
  per-ring ordering checks and no global-order assumption, run in a dedicated TSan CI job.
- **Adapter-zone drain (#38)**: `RingSinkAdapter` copies each drained record into an owning
  `LogRecord` before acknowledging, forwards it to existing sinks with its truncation flag and an
  explicit unavailable-time marker, and exposes per-ring refusal counts.
- **Boundary checks (#39)**: after every build with tests, CTest verifies the compiler-reported
  module imports and evaluated CMake links against a reviewed policy, scans governed sources for
  forbidden constructs, and scans governed objects for allocation and exception symbols, each with
  negative controls. [`docs/governed-evidence.md`](docs/governed-evidence.md) states what these
  checks do and do not establish.
- **Standalone consumption and migration (#40)**: the same core-only program builds and runs in-tree
  and against the installed package while linking only `mddlog::core`;
  [`docs/migration/governed-core.md`](docs/migration/governed-core.md) covers the moved imports,
  capacities, admission and host time.

### Foundations (#5, #7)

- Reproducible builds with CMake presets and a pinned toolchain matrix, SpecLab scenario tests
  discovered per scenario by CTest, and CI on Linux GCC 16.1, Linux Clang 21 with libc++, macOS
  arm64 Clang 21.1.8 and Windows MSVC, plus ASan/UBSan (#5).
- MduX conventions: `LogLevel` enumerators renamed `Trace`…`Audit`, and reproducible
  clang-format and clang-tidy checks in CI (#7).

### Known limits, stated because they are easy to mistake for defects

- **Not certified or validated.** Nothing here qualifies mddlog as medical-device software; an
  allocation-free core makes integration under strong constraints easier, it does not qualify it.
- **The allocating logger is unchanged.** `SimpleLogger` still queues records in an unbounded,
  mutex-protected queue and offers no real-time guarantee. The governed ring is a separate path
  drained by `RingSinkAdapter`; it is not wired into `SimpleLogger`.
- **`RingSinkAdapter::drainOnce()` does not isolate one ring's failure.** An exception from one
  ring's copy or acknowledgement ends the pass, so later rings are not drained in it and the count
  already delivered is not returned.
- **Sinks do not preserve every field.** `ConsoleSink` is the only implemented sink and renders a
  human-readable line; it omits `correlationId` and `complianceStandard`. `SimpleLogger::flush()`
  ignores a sink's flush failure. File, network and audit sinks, formatters and the audit model of
  ADR-002 to ADR-004 are planned (epics #9, #10, #11).
- **Governed evidence is scoped, not a proof.** The source check is lexical, the symbol scans cover
  the inspected objects and representative template instantiations only, and neither establishes
  absence of blocking or a worst-case execution time. `-fno-exceptions` is not enabled or promised
  with `import std`. ThreadSanitizer runs the RingLog scenarios only.
- **`MalformedTime` is unreachable by design.** `RawTime` has only an available and an unavailable
  state (ADR-001 Decision 7); the reason stays in the enumeration for a future time representation.
- **A file importing `mddlog.core.*` directly must link `mddlog::core`,** even when it also links
  `mddlog::mddlog`: with GCC 16.1 and CMake 4.1, modules reached only transitively are missing from
  its module mapper.
- **Toolchain window.** `import std` support is experimental and qualified only for CMake 4.0 to 4.3,
  Ninja generators, GCC 16.1+, Clang 20+ (21 in CI) and MSVC 17.14+.
