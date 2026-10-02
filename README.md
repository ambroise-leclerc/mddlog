# mddlog — C++23 logging for medical device software

**mddlog** is an experimental C++23 modules library for diagnostic logging and bounded, in-memory audit events in medical device software and other regulated embedded systems. It gives developers explicit admission results, per-producer event ordering, and a separately reviewable core. The repository also publishes the design decisions and scoped verification evidence behind those choices.

[Architecture decisions](docs/adr/README.md) · [Governed-core evidence](docs/governed-evidence.md) · [Audit admission guide](docs/migration/audit-admission.md) · [Examples](examples/) · [Releases](https://github.com/ambroise-leclerc/mddlog/releases)

> **Evaluation status:** mddlog is not certified or independently validated for use in a medical device. In-memory admission and hand-off are not durable audit storage. Manufacturers must assess the library in their own system, risk management, software lifecycle, and quality management processes.

## Why this project exists

A diagnostic message, an admitted audit event, and a durable record answer different questions. mddlog keeps those boundaries visible:

| Path | Available now | Important limit |
| --- | --- | --- |
| Governed diagnostic core (`mddlog::core`) | Fixed-capacity `GovernedRecord` and single-producer/single-consumer `RingLog`; host-supplied time; explicit admission and truncation results | No sink, persistence, or whole-system timing guarantee |
| Runtime audit core (`mddlog::core`) | `AuditEvent` and bounded `AuditRing`; exact identifiers, category/phase, optional requirement and risk references, stream identity, and assigned sequence | Admission means the event is in memory; a full ring refuses new events |
| Public audit API (`mddlog::mddlog`) | `SimpleLogger::logAudit(AuditInput)` and `Log::logAudit(AuditInput)` return the ring admission result after `setAuditRing()` | A bound ring must outlive its logger binding; `Log::shutdown()` clears the binding |
| Audit hand-off (`mddlog::mddlog`) | `AuditSinkAdapter` forwards events to an `AuditSink`, reports hand-off health, and acknowledges accepted events | Sink acceptance does not establish durable storage; downstream loss must be reported by the host |
| Diagnostic adapter (`mddlog::mddlog`) | `SimpleLogger`, `LogRecord`, `ConsoleSink`, and the `Log` convenience facade | Its asynchronous queue allocates and is unbounded; it is not the governed path |

The audit ring has **one producer and one consumer**. The host supplies a distinct stream identity for each producer instance and boot session, and owns its response to refusal, hand-off failure, and power loss. See the [admission and delivery contract](docs/migration/audit-admission.md).

## Start with the bounded audit path

This example admits one event into memory and checks the result. It does not create a persistent audit trail.

```cpp
import mddlog.core.auditring;

using namespace mddlog::core;

AuditRing<64> audit{"device_789:boot_42:operator"};
AuditInput input{
    .category = AuditCategory::RiskControl,
    .phase = AuditPhase::Requested,
    .time = RawTime::unavailable(),
    .action = "EMERGENCY_SHUTDOWN",
    .actor = "operator_42",
    .target = "pump_7",
    .riskRef = "RISK_17",
    .correlationId = "device_789:boot_42:input:41",
    .sourceSequence = 41,
    .detail = "Shutdown requested",
};

auto result = audit.tryRecord(input);
if (!result.wasAdmitted()) {
    // Apply the host's refusal policy; inspect result.refusal().
}
```

Link a core-only CMake consumer with `mddlog::core`. For diagnostic logging and audit hand-off, link `mddlog::mddlog`. The full target depends on the core; the core does not import adapters or sinks.

```cmake
add_subdirectory(mddlog)
target_link_libraries(your_target PRIVATE mddlog::core)
```

The public `logAudit(AuditInput)` API introduced in #58 uses the same admission contract; bind a host-owned ring with `setAuditRing()` and clear the binding before destroying the ring. After `Log::shutdown()` and a later initialization, bind it again. `LogLevel::Audit` and the old positional audit overloads have been removed. See the [audit API migration guide](docs/migration/audit-admission.md) for sink ownership and failure handling.

For a diagnostic logger, see [basic_usage.cpp](examples/basic_usage.cpp). For draining governed records to sinks, see the [governed-core migration guide](docs/migration/governed-core.md).

For independently enabled diagnostic groups and synchronous text callbacks, use
`mddlog.adapter.textlogger`. The [text logger guide](docs/migration/text-logger.md) describes its
rendering contract.

## Regulatory context and evidence

The following standards describe processes and responsibilities for medical device development. mddlog provides building blocks and review material; using it does not establish conformity with any standard or regulation.

| Reference | Relevant project material | What remains with the manufacturer |
| --- | --- | --- |
| [IEC 62304:2006 + AMD1:2015](https://webstore.iec.ch/en/publication/22790), medical device software life cycle processes | [ADRs](docs/adr/README.md), versioned source, [tests](tests/), and [governed-boundary checks](docs/governed-evidence.md) | Software lifecycle activities, system verification and validation, configuration and problem resolution |
| [ISO 14971:2019](https://www.iso.org/standard/72704.html), risk management for medical devices | Optional `riskRef` and `requirementRef` identifiers on `AuditEvent`; explicit refusal and hand-off results | Hazard analysis, risk controls, effectiveness checks, and risk management records |
| [ISO 13485:2016](https://committee.iso.org/standard/59752.html), medical device quality management systems | Reviewable changes and documented design decisions | Quality management system, document control, and record retention |

The [evidence note](docs/governed-evidence.md) states exactly what the module graph, source, allocation-symbol, and exception-symbol checks cover and where their conclusions stop. The [scenario evidence](docs/audit-scenario-validation.md) describes tested audit-field and action-transition cases. These are engineering checks, not a certification dossier.

### Delivery and roadmap

- **Implemented and accepted:** bounded audit admission, per-stream sequence, explicit refusal, and in-memory sink hand-off with health reporting ([ADR-002](docs/adr/ADR-002-regulatory-audit-event-model.md); [epic #9](https://github.com/ambroise-leclerc/mddlog/issues/9)).
- **Implemented and accepted:** application integration building blocks: synchronized sink registry, bounded transport consumer and diagnostic text logger, with WebFront as the reference consumer verified in its own repository ([ADR-003](docs/adr/ADR-003-application-integration-and-sink-ownership.md); [epic #10](https://github.com/ambroise-leclerc/mddlog/issues/10)).
- **Planned:** durable storage, tamper evidence, and recovery semantics ([ADR-004](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md); [epic #11](https://github.com/ambroise-leclerc/mddlog/issues/11)).

## Build and verify

CMake is the source of truth for supported compilers and module files. The admitted toolchain floors are GCC 16.1, upstream Clang 20, or MSVC 19.40 (Visual Studio 2022 17.10), with CMake 4.0–4.3 and Ninja. An admitted version is not necessarily a tested configuration; `import std` support depends on the exact compiler, standard library, and CMake combination.

```bash
cmake -S . -B build -G Ninja -DMDDLOG_BUILD_EXAMPLES=OFF -DMDDLOG_BUILD_TESTS=OFF
cmake --build build --parallel
```

The CI configurations are:

| Platform | Compiler and standard library | CMake |
| --- | --- | --- |
| Windows x64 | MSVC 19.40+ | 4.1.1 |
| Linux x86_64 | GCC 16.1.0 / libstdc++ | 4.1.1 |
| Linux x86_64 | upstream Clang 21 / libc++ | 4.3.1 |
| macOS arm64 | upstream Clang 21.1.8 / libc++ | 4.3.1 |

Use the matching preset in [CMakePresets.json](CMakePresets.json) to build examples and run CTest, for example:

```bash
cmake --preset ninja-gcc
cmake --build --preset ninja-gcc
ctest --preset ninja-gcc --output-on-failure
```

Tests fetch a pinned [SpecLab](https://github.com/ambroise-leclerc/SpecLab) revision only when enabled. `SourceTreeCoreConsumer` and `InstallTreeCoreConsumer` separately exercise the core-only target. CI also runs sanitizer and governed-boundary checks; consult [the evidence note](docs/governed-evidence.md) for their scope and limits. A successful configure step alone is not a build or test result.

## License and participation

mddlog is offered under the [European Union Public Licence 1.2](LICENSE) or separate commercial terms; see [LICENSING.md](LICENSING.md). [CONTRIBUTING.md](CONTRIBUTING.md) explains the invitation-only contribution process and review rules. For questions or defects, use [GitHub Issues](https://github.com/ambroise-leclerc/mddlog/issues).
