# mddlog - Medical Device Logger

A modern C++23 modules logging experiment shaped by IEC 62304, ISO 13485, and ISO 14971 concerns; it is not certified or validated for medical-device use.

⚠️ EXPERIMENTAL PROJECT WARNING

This project is an experimental early evaluation of C++23 modules feasibility for medical device
logging. It represents an attempt to leverage C++23 (and emerging C++26 safety evolutions) for
medical device software development.

Current Status:

- C++23 modules support requires cutting-edge toolchains (GCC 16.1+, MSVC 17.14+, Clang 20+)
- CMake 4.0-4.3 experimental support for `import std;`
- The reference/CI-verified configurations are the four listed under [Tested
  Configurations](#tested-configurations) below; other toolchain/version combinations may work
  but have not been exercised in CI.
- Medical device compliance framework (audit trail structure, risk-level fields) is conceptual: it
  models the shape of the data IEC 62304/ISO 13485/ISO 14971 processes care about, but nothing here
  has been certified or independently validated against those standards.
- Exploring modern C++ safety features for medical device reliability

Not recommended for production use. This project serves as a technical proof-of-concept for modern
C++ module systems in a regulated-software-shaped domain, not as validated medical device software.

## Core Design Principles

### Medical Device Compliance
- **IEC 62304** (Medical device software lifecycle processes)
- **ISO 13485** (Quality management systems for medical devices) 
- **ISO 14971** (Risk management for medical devices)

### Technical Requirements
- **Pure C++23 modules library** - No legacy headers, modern module system
- **No dependencies** except `import std` (the test suite additionally depends on
  [SpecLab](https://github.com/ambroise-leclerc/SpecLab), pinned and test-only - see
  [Testing](#testing))
- **Cross-platform**: MSVC 17.14+, GCC 16.1+, Clang 20+ - see [Tested Configurations](#tested-configurations)
- **Thread-safe delivery paths**: `SimpleLogger` uses a mutex-protected, unbounded asynchronous
  queue; the governed core provides a separate bounded SPSC ring. Neither is a general real-time
  guarantee, and the adapter queue may grow without bound if sinks cannot keep up.
- **Zero-macro design** using modern C++23 features

## Architecture Overview

### C++23 Infrastructure
- **Modules-first design**: All components as `.cppm` modules
- **Import std**: Leveraging standard library modules
- **Thread safety**: Using `std::thread`, `std::mutex`, and atomic operations
- **Source location**: Automatic debug information with `std::source_location`, captured at the
  application's actual call site (forwarded explicitly through every Logger method - see
  `include/mddlog/adapter/Logger.cppm`)

### Key Features

#### 1. Core Logging Infrastructure
- **Structured logging** with `LogRecord` containing timestamp, severity, category, message, and metadata
- **Multiple severity levels**: Trace, Debug, Info, Warn, Error, Fatal, Audit
- **Thread-safe operations** using `std::thread`, `std::mutex`, and atomic operations

#### 2. Medical Device Compliance
- **Audit Trail**: `logAudit()` records intentionally bypass the logger's own
  enablement/minimum-level filtering, so a reconfigured or disabled logger cannot silently erase
  compliance evidence (see `AuditPolicySpec.cpp` for the behavioral contract). Persistent,
  tamper-evident storage of those records is *not* implemented - only the in-process record shape
  and delivery policy are.
- **Risk Management**: `LogRecord` carries a risk-level field associated with audit events, for
  callers to populate per their own ISO 14971 process *(structural support only)*
- **Lifecycle Logging**: Development, verification, and validation event tracking *(planned)*
- **Regulatory Reporting**: Automated compliance report generation *(planned)*

#### 3. Advanced Sink System
- **FileSink** with log rotation and compression *(planned)*
- **NetworkSink** for remote monitoring systems *(planned)*
- **AuditSink** for tamper-proof audit records *(planned)*
- **ConsoleSink** with colors and statistics *(implemented)*
- **Configurable buffering** and batching strategies *(planned)*

#### 4. Custom Formatters *(planned)*
- **JsonFormatter** for structured data
- **XmlFormatter** for medical device standards
- **MedicalFormatter** with compliance-specific fields

#### 5. Performance & Monitoring
- **Asynchronous delivery** via a mutex-protected, unbounded queue and a dedicated worker thread
  (not lock-free - see the note under [Technical Requirements](#technical-requirements))
- **Memory pool allocation** for zero-allocation logging *(planned)*
- **Per-sink statistics**: records written/dropped, bytes written, flush count, write time
  (`LogStatistics`, exposed via `Sink::getStatistics()`)
- **Real-time constraints**: not currently guaranteed; see the unbounded-queue limitation above

## Quick Start

### Basic Usage

```cpp
import std;
import mddlog;

using namespace mddlog;

int main() {
    // Create logger with async processing
    auto logger = std::make_unique<SimpleLogger>("MedicalDevice", true);
    
    // Add console sink with colors
    auto consoleSink = createConsoleSink(true, true);
    logger->addSink(consoleSink);
    
    // Set minimum log level
    logger->setMinLevel(LogLevel::Info);
    
    // Basic logging
    logger->info("Medical device system started");
    logger->warn("Temperature sensor reading elevated");
    logger->error("Failed to connect to monitoring system");
    
    return 0;
}
```

### Medical Compliance Logging

```cpp
// Medical compliance logging with user/device context
logger->logMedical(
    LogLevel::Info,
    "Patient monitoring session started",
    "patient_monitor",
    "user123",           // User ID
    "session_456",       // Session ID
    "device_789"         // Device ID
);

// Audit trail logging
logger->logAudit(
    "User accessed patient data",
    "DATA_ACCESS",       // Event type
    "user123",           // User ID
    "device_789",        // Device ID
    "LOW"                // Risk level
);
```

> **Migrating from `LogLevel::INFO`-style names?** The enumerators are now CamelCase
> (`LogLevel::Info`); see the [migration table](docs/migration/loglevel-rename.md).

## Module Architecture

Two CMake targets, with a one-way dependency: `mddlog-core` (alias `mddlog::core`) contains only
governed modules (`mddlog.core.*`) and no sink or adapter module; `mddlog` (alias `mddlog::mddlog`)
links `mddlog-core` and adds the adapter and sink modules. A consumer that only needs the governed
core (no `SimpleLogger`, no sinks) can link `mddlog::core` alone - see ADR-001 Decision 6. The core
now includes the fixed-capacity `mddlog.core.record` value type, its supporting string and result
types, and the bounded SPSC `mddlog.core.ring`. `mddlog.adapter.ringdrain` copies governed records
out of ring spans before acknowledgement and forwards them to existing sinks. The existing
`SimpleLogger` still uses its allocating record and unbounded queue.

Core-only clients can import `mddlog.core.record` and `mddlog.core.ring`, then link only
`mddlog::core`. The same write/drain/acknowledge program is tested both as an in-tree target and
against the installed package, with separate CTest results. See the
[governed-core migration guide](docs/migration/governed-core.md) for imports, capacities,
admission, host-supplied time and CMake examples.

### Core Components

```
mddlog/
├── core/
│   ├── InlineString.cppm     # Fixed-capacity string storage (mddlog-core)
│   ├── LogLevel.cppm         # Severity levels and utilities (mddlog-core)
│   ├── WriteResult.cppm      # Admission and truncation result (mddlog-core)
│   ├── Record.cppm           # Governed record and host-supplied time (mddlog-core)
│   └── Ring.cppm             # Fixed-capacity SPSC queue (mddlog-core)
├── adapter/
│   ├── LogRecord.cppm        # Structured log record (allocating; mddlog)
│   ├── RingDrain.cppm        # Governed ring-to-sink bridge (mddlog)
│   └── Logger.cppm           # Main logger implementation (mddlog)
├── sinks/
│   ├── Sink.cppm             # Base sink interface (mddlog)
│   └── ConsoleSink.cppm      # Console output sink (mddlog)
├── Log.cppm                  # Static Log:: convenience wrapper around a global logger (mddlog)
└── mddlog.cppm               # Main module with exports (mddlog)
```

### Log Record Structure

The existing adapter `LogRecord` contains:

- **Core Information**: Timestamp, level, message, category, thread ID
- **Source Location**: File, line, function (automatic with `std::source_location`)
- **Medical Context**: User ID, session ID, device ID, operation ID
- **Audit Information**: Event type, risk level, compliance standard
- **Metadata**: Custom key-value pairs
- **Performance**: Processing time tracking

The separate governed `GovernedRecord` owns a bounded message and component, operation and
correlation identifiers. It captures a host-supplied raw time and source location without reading a
clock; its `assign()` result reports admission and message truncation. `RingLog<Capacity>` stores
these records inline, refuses writes when full, and exposes one or two read-only spans until the
consumer explicitly acknowledges them. Each ring has one producer and one consumer; separate rings
have no global ordering. `RingSinkAdapter` is the separate adapter-zone path for one or more such
rings, even when their compile-time capacities differ. It copies each drained record into an owning
`LogRecord` before acknowledging and dispatching to sinks, including the message-truncation flag
and an explicit unavailable-time state.
Existing sinks do not preserve every governed field in their output; the console sink adds a
`[truncated]` marker but remains a human-readable, non-round-trippable view.

## Medical Device Compliance

The governed boundary has separate dependency, source, allocation-symbol and exception-symbol
checks. See [the evidence note](docs/governed-evidence.md) for how to run them, the covered
toolchains, and what their results do and do not establish.

### IEC 62304 Requirements

MddLog addresses IEC 62304 requirements for medical device software:

- **Software Lifecycle Processes**: Logging of development, verification, and validation activities
- **Risk Management**: Integration with ISO 14971 risk management processes
- **Configuration Management**: Change tracking and version control logging
- **Problem Resolution**: Error tracking and resolution logging

### ISO 13485 Requirements

MddLog supports ISO 13485 quality management requirements:

- **Document Control**: Audit trail for document changes
- **Management Responsibility**: Executive action logging
- **Resource Management**: Training and competency logging
- **Product Realization**: Manufacturing and testing process logging
- **Measurement and Improvement**: Quality metrics and corrective action logging

### ISO 14971 Risk Management

- **Hazard Identification**: Systematic hazard logging and tracking
- **Risk Analysis**: Risk assessment documentation and monitoring
- **Risk Control**: Risk mitigation measure logging
- **Post-Market Surveillance**: Risk monitoring in deployed systems

### Audit Trail Features

- **Tamper-proof records** with cryptographic signatures *(planned)*
- **User authentication** and authorization logging
- **Data access tracking** with privacy compliance
- **System configuration** change logging
- **Emergency procedures** and incident logging

## Building and Integration

### CMake Integration

```cmake
# Add MddLog to your project
add_subdirectory(mddlog)

# Link to your target
target_link_libraries(your_target PRIVATE mddlog::mddlog)

# Set C++23 standard
set_property(TARGET your_target PROPERTY CXX_STANDARD 23)
```

### Compiler Requirements

These are admission floors checked by `CMakeLists.txt`, not a claim that every version above them
is verified - see [Tested Configurations](#tested-configurations) for what CI actually exercises.

- **MSVC**: 17.14+ (Visual Studio 2022 version 17.10+)
- **GCC**: 16.1+ (GCC 15 cannot build the SpecLab-based test suite; see `tests/CMakeLists.txt`)
- **Clang**: 20.0+ (upstream Clang only - AppleClang is rejected on macOS)
- **CMake**: 4.0-4.3 (4.4+ is rejected until its `import std` gate has been reviewed)
- **Ninja** is required; other generators do not implement C++ modules for this toolchain matrix

### C++23 Features Used

- **Modules**: `import std` and custom modules for faster compilation
- **Source Location**: Automatic source tracking for debugging, captured at the caller's site
- **Concepts**: Type constraints and requirements for type safety

## Tested Configurations

The four configurations below are exercised in CI (`.github/workflows/`) on every push, from the
matching CMake preset in `CMakePresets.json`:

| Target | Compiler / standard library | Tooling |
| --- | --- | --- |
| Windows x64 | MSVC (from the installed VS developer environment), 19.40+ | CMake 4.1.1, Ninja, CTest |
| Linux x86_64 / GCC | `gcc:16.1.0` container, libstdc++ | CMake 4.1.1, Ninja, CTest |
| Linux x86_64 / Clang | Upstream Clang 21, libc++/libc++abi | CMake 4.3.1, Ninja, CTest |
| macOS arm64 | Upstream LLVM/Clang 21.1.8 exactly, libc++ | CMake 4.3.1 exactly, Ninja, CTest, `macos-15` runner |

A dedicated `sanitizers.yml` workflow additionally runs the full test suite under
AddressSanitizer + UndefinedBehaviorSanitizer on GCC 16.1 Debug. AppleClang, Intel macOS, and GCC
on macOS are explicitly rejected by the top-level `CMakeLists.txt`, not merely untested.

The Linux/GCC and Linux/Clang lanes (including the ASan/UBSan build, and the ccache
module-integrity regression) have additionally been run and pass locally against `gcc 16.1.0` and
upstream `clang 21.1.8`/`libc++-21-dev`. Windows/MSVC and macOS/Clang have not been exercised
outside CI.

## Testing

Tests are written as [SpecLab](https://github.com/ambroise-leclerc/SpecLab) Given/When/Then
specifications (`tests/spec/*.cpp`), pinned to an exact upstream commit and fetched only when
`MDDLOG_BUILD_TESTS=ON` - SpecLab is never a dependency of the installed library. Each scenario is
registered as its own CTest entry (`ctest -R "<scenario name>"` selects one) via
`cmake/MddlogTestDiscovery.cmake`, which drives SpecLab's own `--list-tests` / `--run=<name>`
contract.

```bash
cmake --preset ninja-gcc      # or ninja-clang / ninja-msvc / ninja-macos-clang
cmake --build --preset ninja-gcc
ctest --preset ninja-gcc --output-on-failure
```

Coverage includes: logger/sink filtering and routing; record fidelity (severity, message,
category, medical/audit fields, producer thread id, caller source location); synchronous and
asynchronous delivery with per-producer ordering; flush-as-barrier and destruction-drains-queue
semantics; concurrent producers under a shared logger; sink failure isolation (a throwing sink
does not block other sinks or crash the async worker, and the failure is recorded explicitly in
that sink's statistics); and the audit-bypass policy described above. `SourceTreeCoreConsumer`
tests a core-only source-tree target. `InstallTreeCoreConsumer` builds and runs the same program
from the installed package using only `mddlog::core`; `InstallTreeConsumer` separately exercises
the full installed `mddlog::mddlog` target.

`RingLog` also has producer/consumer tests on separate threads: controlled saturation and reuse,
plus several producer-owned rings drained by one consumer, checking order within each ring only.
The separate GCC 16.1 ThreadSanitizer CI job builds the `import std` module stack and runs the
RingLog scenarios without suppressions. A clean TSan run reports no race in those executions; it
does not prove timing bounds or absence of allocation.

## Examples

See the `examples/` directory for usage examples:

- `basic_usage.cpp` - `SimpleLogger` with a console sink, medical/audit logging, and multi-threaded usage
- `simple_usage.cpp` - The static `Log::` convenience wrapper

## Building from Source

Building requires Ninja and one of the toolchains listed under [Tested
Configurations](#tested-configurations) - the top-level `CMakeLists.txt` rejects any other
generator or an unsupported compiler/CMake combination with an explicit error rather than failing
deep inside module scanning.

```bash
# Configure via the preset matching your platform/compiler (see CMakePresets.json)
cmake --preset ninja-gcc      # Linux/GCC; also: ninja-clang, ninja-msvc, ninja-macos-clang

# Build
cmake --build --preset ninja-gcc

# Run the examples
./build-gcc/examples/basic_usage
./build-gcc/examples/simple_usage

# Run the test suite (see Testing above)
ctest --preset ninja-gcc --output-on-failure
```

A plain `cmake -S . -B build -G Ninja ...` also works if you pass the equivalent cache variables
by hand; the presets exist so CI and local builds cannot silently drift apart.

## License

This project is available under the [European Union Public Licence 1.2](LICENSE), or under separate
commercial terms. See [LICENSING.md](LICENSING.md).

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) for details on our code of conduct and the process for submitting pull requests.

[`AGENTS.md`](AGENTS.md) is the canonical, tool-neutral contributor guide; tool-specific assistant
files such as `MISTRAL.md`, `VIBE.md` ou `CLAUDE.md` are intentionally ignored.

## Medical Device Certification

This library is designed to support medical device certification processes but is not itself certified. Users are responsible for validation and verification according to their specific regulatory requirements.

## Support


For questions, issues, or contributions, please visit our [GitHub repository](https://github.com/ambroise-leclerc/mddlog).
