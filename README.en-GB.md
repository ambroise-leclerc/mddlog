# mddlog

**C++23 logging library** for critical medical devices and regulated or safety-critical software.

[🇫🇷 Français](README.md) · 🇬🇧 English · [🇪🇸 Español](README.es.md)

```cpp
import std;
import mddlog;

int main() {
    // Configure at the application composition point.
    mddlog::SimpleLogger destination("pump", false);
    destination.addSink(mddlog::createConsoleSink());
    const auto context = mddlog::DiagnosticContext::create({
        .component = "pump", .operationId = "prime", .correlationId = "call-7"
    });
    if (!context) return 1; // Handle construction refusal before creating the binding.
    mddlog::DiagnosticBinding logger(destination, *context);

    // In business code: no repeated context fields.
    logger.info("Pump ready");
    logger.error("Occlusion detected on line A");
    logger.debugLazy([] { return std::format("state={}", 42); });
}
```

- **Diagnostic logs**: the usual levels, sinks and a ready-to-use `Log` facade.
- **Bounded, allocation-free core**: fixed capacity, explicit refusal rather than silent overwriting.
- **Audit trail**: events kept apart from logs, persisted, chained and verifiable against an independent anchor.

[Architecture decisions](docs/adr/README.md) · [Audit persistence validation](docs/audit-persistence-validation.md) · [Audit admission guide](docs/migration/audit-admission.md) · [Audit ledger guide](docs/migration/audit-ledger.md) · [Governed-core evidence](docs/governed-evidence.md) · [Changelog](CHANGELOG.md) · [Releases](https://github.com/ambroise-leclerc/mddlog/releases)

The French [README](README.md) is the reference version.

## What is new in version 0.3.0

v0.3.0 delivers the contextual API from [#113](https://github.com/ambroise-leclerc/mddlog/issues/113), designed in [ADR-005](docs/adr/ADR-005-contextual-logging-api.md). Configuration stays at the composition point; business functions receive a reusable binding.

- **Diagnostics**: `DiagnosticContext` owns component, operation and correlation; `DiagnosticBinding` preserves the actual caller source with `SimpleLogger` or `TextLogger`.
- **Expensive messages**: `debugLazy(factory)` and `logLazy(level, factory)` construct the message after the adapter filter.
- **Governed paths**: `GovernedBinding` retains explicit time and `WriteResult`; `AuditDescription`, `AuditContext` and `AuditBinding` separate invariants from each event’s phases and facts.
- **Evidence and migration**: five before/after usages, a local stock component and source/installed consumers. Local integration is accepted; independent application #122 and the 1.0 freeze #121 remain open.

The low-level APIs and global `Log` facade remain available. See the [context guide](docs/migration/contextual-logging.md) and [integration evidence](docs/contextual-api-integration.md).

## Capabilities delivered in 0.2.0

v0.1.0 delivered a bounded, allocation-free logging core (ADR-001). v0.2.0 adds three complete capabilities.

### 1. Audit events, separate from diagnostic logs — [ADR-002](docs/adr/ADR-002-regulatory-audit-event-model.md)

- **`AuditEvent`** carries exact identifiers (action, actor, target, requirement and risk references, correlation), a category, a phase (requested, confirmed, executed, failed), a host-supplied time and a stream identity. An invalid identifier is refused, never truncated; only the `detail` text may be shortened, with a flag.
- **`AuditRing<N>`** admits an event into bounded memory and assigns it a per-stream sequence number. When full, it refuses rather than overwrites, without consuming a sequence.
- **`AuditSinkAdapter`** hands admitted events to an `AuditSink`, releases only what was accepted, keeps the rest for a retry, and publishes its own health: handed off, failures, losses after admission, pending events.
- **`logAudit(AuditInput)`** on `SimpleLogger` and `Log` returns the admission result, whatever the diagnostic filters.

### 2. Building blocks for integrating mddlog into an application — [ADR-003](docs/adr/ADR-003-application-integration-and-sink-ownership.md)

- **`SinkRegistry`**: synchronised callback registration through explicit handles, with safe removal, including from the callback itself.
- **`TransportConsumer`**: bounded rings consumed on one thread, independent health, isolation of a failing transport, and suppression of reentrant logging loops.
- **`TextLogger`**: a diagnostic text log with independently enabled groups, caller source and separate dumps.
- WebFront adopts and verifies them in its own repository.

### 3. Audit persistence, with tamper evidence through anchoring — [ADR-004](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md)

- **Canonical format and chaining**: each event is encoded under a versioned byte contract and chained with SHA-256 within its stream. Reference vectors fix every byte, and every CI toolchain checks them.
- **Storage and durable confirmation**: `PersistingAuditSink` writes append-only segments to a `StorageMedium`. An event is reported "durably confirmed" only after a `sync` the medium confirmed, for a complete prefix of the stream. Every storage failure is visible in the sink's health and is never presented as an audit event.
- **Anchoring and verification**: an `AnchorProvider`, outside the log, keeps the position and digest reached. `LogVerifier` reads the whole medium back and returns, for each stream, an explicit verdict: *Anchored* with its range, *Unanchored*, *anchor unavailable*, *Incomplete*, *Altered*, *Rolled back*, *Conflict*, *Retired*, *Cannot verify* or *Inconsistent*. Never a bare "valid".
- **Rollback detected**: the position the reader retains off the device (`RetainedPosition`) reveals an old log restored together with its old anchor.
- **Restart, rotation and retention**: a ledger records every start, every stream opened or closed and every removal. Recovery recomputes the chain state and checks it against the anchor. Rotation removes whole segments, never past the anchor, after durably confirming its record. The reader reports every restart, every removal and every remnant of a power cut as a boundary, never as continuity.
- **End-to-end validation**: every validation scenario of ADR-004, from a rewrite with recomputed digests to a power cut during a `sync`, runs from the producer to the reader's report. The [validation report](docs/audit-persistence-validation.md) maps each criterion to its tests and states the limits.

### Breaking changes

- `LogLevel::Audit` and the old positional `logAudit(...)` overloads are removed: use `logAudit(AuditInput)` ([guide](docs/migration/audit-admission.md)).
- Actions beginning with `mddlog.` are reserved for the persistence ledger: producer admission refuses them (`AuditRefusalReason::ReservedAction`).

## What each path provides

| Path | What it provides | Limit to know |
| --- | --- | --- |
| Governed diagnostic core (`mddlog::core`) | Fixed-capacity `GovernedRecord` and single-producer/single-consumer `RingLog`; host-supplied time; explicit admission and truncation results | No sink, no persistence, no system-wide timing guarantee |
| Audit core (`mddlog::core`) | Bounded `AuditEvent` and `AuditRing`; exact identifiers, category and phase, stream identity and assigned sequence | Admitted means in memory; a full ring refuses new events |
| Contexts and bindings (`mddlog::core`, adapters through `mddlog::mddlog`) | `DiagnosticContext`, `GovernedBinding`, `AuditDescription`, `AuditContext`, `AuditBinding` and `DiagnosticBinding` | Owned contexts, borrowed destinations; explicit governed refusal and time; one producer per ring |
| Public audit API (`mddlog::mddlog`) | `SimpleLogger::logAudit(AuditInput)` and `Log::logAudit(AuditInput)` return the admission result after `setAuditRing()` | The bound ring must outlive its binding; `Log::shutdown()` clears the binding |
| Audit hand-off (`mddlog::mddlog`) | `AuditSinkAdapter` hands off to an `AuditSink`, publishes its health and acknowledges what was accepted | One consumer thread; acceptance by a sink is not durable storage |
| Audit persistence (`mddlog::mddlog`) | `PersistingAuditSink` stores, confirms durably, keeps a ledger, rotates and retains; `LogVerifier` returns a verdict per stream | Durability depends on the medium; tamper evidence depends on an independent anchor; nothing is signed |
| Application integration (`mddlog::mddlog`) | `SinkRegistry`, `TransportConsumer` and `TextLogger` | `TextLogger` renders and calls back synchronously, allocating |
| Diagnostic adapter (`mddlog::mddlog`) | `SimpleLogger`, `LogRecord`, `ConsoleSink` and the `Log` facade | Its asynchronous queue allocates and is unbounded: it is not the governed path |

Every ring has **one producer and one consumer**. The host supplies a distinct stream identity for each producer and each boot session, and decides how it responds to a refusal, a hand-off failure or a power loss.

## Getting started

### Admit an audit event with a reusable context

```cpp
import mddlog.core.auditbinding;

using namespace mddlog::core;

AuditRing<64> audit{"device_789:boot_42:operator"};
const auto description = AuditDescription::create({
    .category = AuditCategory::RiskControl,
    .action = "EMERGENCY_SHUTDOWN",
    .riskRef = "RISK_17",
});
const auto context = AuditContext::create({
    .actor = "operator_42", .target = "pump_7",
    .correlationId = "device_789:boot_42:input:41",
});
if (!description || !context) {
    // Handle refused identifiers before emitting any event.
    return 1;
}
AuditBinding events(audit, *description, *context);

const auto result = events.record(AuditPhase::Requested, RawTime::unavailable(),
                                 {.detail = "Shutdown requested", .sourceSequence = 41});
if (!result.wasAdmitted()) {
    // Apply the host’s refusal policy; inspect result.refusal().
    return 2;
}
```

The description and context are copied; the binding borrows the ring. Phase and time remain explicit at every call. `Requested` declares a request: the binding does not execute the action and its destructor emits nothing. Admission is an in-memory copy, not durable confirmation.

### Persist and verify

The integrator supplies the medium and the anchor provider: the medium must meet ADR-004's storage contract (Decision 9), and the provider must be independent of the log (Decision 7).

```cpp
import mddlog.adapter.auditdrain;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;

using namespace mddlog::adapter;

MyStorageMedium medium;    // implements StorageMedium
MyAnchorProvider provider; // implements AnchorProvider, outside the log

StorageConfig config;
config.segmentSize        = 4096;
config.segmentCount       = 256;
config.maxProducerStreams = 4;
config.ledger             = LedgerConfig{.streamId = "device_789:boot_42:ledger"};
config.provider           = &provider;

auto sink = PersistingAuditSink::create(medium, std::move(config));
if (!sink) {
    // Configuration refused: inspect sink.error().
}

AuditSinkAdapter adapter;
(void)adapter.addRing(audit); // the AuditRing of the previous example
adapter.setSink(*sink);
(void)adapter.drainOnce();    // on the consumer thread
(void)(*sink)->advanceAnchor("device_789:boot_42:operator");

RetainedPosition retained; // kept by the reader, off the device
LogVerifier verifier{medium, provider, retained};
const LogReport report = verifier.verify();
```

A CMake consumer that needs only the core links `mddlog::core`; for audit hand-off and persistence, diagnostic logging and integration, it links `mddlog::mddlog`. The full target depends on the core; the core imports no adapter or sink.

```cmake
add_subdirectory(mddlog)
target_link_libraries(your_target PRIVATE mddlog::mddlog)
```

Further reading: [audit admission and hand-off](docs/migration/audit-admission.md), [ledger, restart, rotation and retention](docs/migration/audit-ledger.md), [text logger](docs/migration/text-logger.md), [transport consumer](docs/migration/transport-consumer.md), [governed core](docs/migration/governed-core.md), and the [basic_usage.cpp](examples/basic_usage.cpp) example.

## What persistence guarantees, and what it does not

- **Tamper evidence, relative to an anchor.** A rewrite, a truncation or an old restored state is reported when it affects records an independent anchor covers, or a position the reader retained. The log remains modifiable by anyone who can write the medium: mddlog does not prevent it.
- **Past the last anchor, nothing is detectable.** Later records are only consistent with each other, and the report says so.
- **No proof of authorship.** Nothing is signed: chaining shows consistency, not who wrote a record. Signing and key management are deferred (ADR-004, Decision 11); candidate export formats are available for review ([ADR-006](docs/adr/ADR-006-audit-tools-and-export.md)).
- **The library ships no real medium or provider.** `InMemoryStorageMedium` and `InMemoryAnchorProvider` are test doubles. Qualifying a suitable medium (file, flash, device log) and provider is the integrator's task.
- **One consumer thread** for the audit adapter, the transport consumer and the persisting sink.

The [validation report](docs/audit-persistence-validation.md) details the coverage demonstrated and its limits.

## Regulatory context and evidence

These standards describe processes and responsibilities in medical device development. mddlog provides building blocks and review material; using it does not establish conformity with any standard or regulation, and the library is neither certified nor validated for any given device. Each manufacturer assesses it in its own system, risk management, software lifecycle and quality management processes.

| Reference | Project material | What remains with the manufacturer |
| --- | --- | --- |
| [IEC 62304:2006 + AMD1:2015](https://webstore.iec.ch/en/publication/22790), medical device software life cycle processes | [ADRs](docs/adr/README.md), versioned source, [tests](tests/), [governed-core checks](docs/governed-evidence.md) and [persistence validation](docs/audit-persistence-validation.md) | Software lifecycle activities, system verification and validation, configuration management and problem resolution |
| [ISO 14971:2019](https://www.iso.org/standard/72704.html), risk management | `riskRef` and `requirementRef` on `AuditEvent`; explicit refusal, hand-off and verification results | Hazard analysis, risk controls, verification of their effectiveness and the risk management file |
| [ISO 13485:2016](https://committee.iso.org/standard/59752.html), quality management systems | Reviewed changes and documented design decisions | Quality management system, document control and record retention |

The [evidence note](docs/governed-evidence.md) states what the module graph, source, allocation-symbol and exception-symbol checks cover. The [scenario evidence](docs/audit-scenario-validation.md) describes the tested audit cases. These are engineering checks, not a certification dossier.

### Deliveries and roadmap

- **Delivered (v0.1.0):** allocation-free bounded logging core ([ADR-001](docs/adr/ADR-001-allocation-free-governed-logging-core.md); epic [#8](https://github.com/ambroise-leclerc/mddlog/issues/8)).
- **Delivered (v0.2.0):** bounded audit admission, per-stream sequence, explicit refusal and hand-off with health ([ADR-002](docs/adr/ADR-002-regulatory-audit-event-model.md); epic [#9](https://github.com/ambroise-leclerc/mddlog/issues/9)).
- **Delivered (v0.2.0):** synchronised sink registry, bounded transport consumer and text logger, with WebFront as the reference consumer ([ADR-003](docs/adr/ADR-003-application-integration-and-sink-ownership.md); epic [#10](https://github.com/ambroise-leclerc/mddlog/issues/10)).
- **Delivered (v0.2.0), evidence under review:** durable confirmation on an eligible medium, tamper evidence relative to an independent anchor, restart, rotation and retention ([ADR-004](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md); epic [#11](https://github.com/ambroise-leclerc/mddlog/issues/11)). Accepting that evidence is a separate review decision.
- **Delivered (v0.3.0):** diagnostic/audit contexts and bindings, lazy filtering and verified usages ([ADR-005](docs/adr/ADR-005-contextual-logging-api.md) ; [#113](https://github.com/ambroise-leclerc/mddlog/issues/113)). Local component integration is accepted; independent application #122, budgets #117, robustness #120 and the freeze #121 remain open.
- **Deferred:** signing and key management ([ADR-004, Decision 11](docs/adr/ADR-004-audit-persistence-and-tamper-evidence.md)); export format acceptance remains open ([ADR-006](docs/adr/ADR-006-audit-tools-and-export.md)).

## Build and verify

CMake is the source of truth for supported compilers and module files. The admitted toolchain floors are GCC 16.1, upstream Clang 20 or MSVC 19.40 (Visual Studio 2022 17.10), with CMake 4.0 to 4.3 and Ninja. An admitted version is not necessarily a tested configuration: `import std` support depends on the exact compiler, standard library and CMake combination.

```bash
cmake -S . -B build -G Ninja -DMDDLOG_BUILD_EXAMPLES=OFF -DMDDLOG_BUILD_TESTS=OFF
cmake --build build --parallel
```

CI configurations:

| Platform | Compiler and standard library | CMake |
| --- | --- | --- |
| Windows x64 | MSVC 19.40+ | 4.1.1 |
| Linux x86_64 | GCC 16.1.0 / libstdc++ | 4.1.1 |
| Linux x86_64 | upstream Clang 21 / libc++ | 4.3.1 |
| macOS arm64 | upstream Clang 21.1.8 / libc++ | 4.3.1 |

Use the matching preset in [CMakePresets.json](CMakePresets.json) to build the examples and run CTest, for example:

```bash
cmake --preset ninja-gcc
cmake --build --preset ninja-gcc
ctest --preset ninja-gcc --output-on-failure
```

Tests fetch a pinned [SpecLab](https://github.com/ambroise-leclerc/SpecLab) revision, only when enabled. `SourceTreeCoreConsumer` and `InstallTreeCoreConsumer` separately exercise the core-only target. CI also runs sanitisers and the governed-core checks; see [the evidence note](docs/governed-evidence.md) for their scope. A successful configuration is neither a build nor a test result.

## Licence and participation

mddlog is offered under the [European Union Public Licence 1.2](LICENSE) or separate commercial terms; see [LICENSING.md](LICENSING.md). [CONTRIBUTING.md](CONTRIBUTING.md) describes the invitation-only contribution process and review rules. For questions or defects, use [GitHub Issues](https://github.com/ambroise-leclerc/mddlog/issues).

### Optional audit tools (#119)

The Linux `mddlog-audit` CLI inspects journals, exports JSON projections or complete
evidence packages, and replays packages with explicit anchor trust and reader-state
provenance. Build with `-DMDDLOG_BUILD_AUDIT_TOOLS=ON`; see [usage](docs/audit-tools.md),
[candidate schemas](docs/audit-export-format.md) and [local verification](docs/audit-tools-validation.md).
Projection/evidence v1 is proposed for maintainer review; no compliance attestation or
self-authenticating package is claimed. Final schema/support acceptance remains #121/#122.
