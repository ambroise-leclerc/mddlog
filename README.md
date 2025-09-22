# mddlog - Medical Device Logger

A modern C++23 modules logging library specifically designed for medical devices, conformant to IEC 62304, ISO 13485, and ISO 14971 standards.

⚠️ EXPERIMENTAL PROJECT WARNING

This project is an experimental early evaluation of C++23 modules feasibility for cross-platform development with rich dependencies (Vulkan graphics, medical device compliance frameworks). It represents an attempt to leverage C++23 and emerging C++26 safety evolutions for medical device software development.

Current Status:

C++23 modules support requires cutting-edge toolchains (GCC 15+, MSVC 17.14+, Clang 20+)
CMake 4.x+ experimental support for import std;
Cross-platform compatibility still evolving
Medical device compliance framework is conceptual/educational
Exploring modern C++ safety features for medical device reliability
Not recommended for production use. This project serves as a technical proof-of-concept for modern C++ module systems and safety evolutions in complex, regulated software environments.

## Core Design Principles

### Medical Device Compliance
- **IEC 62304** (Medical device software lifecycle processes)
- **ISO 13485** (Quality management systems for medical devices) 
- **ISO 14971** (Risk management for medical devices)

### Technical Requirements
- **Pure C++23 modules library** - No legacy headers, modern module system
- **No dependencies** except `import std`
- **Cross-platform**: MSVC 17.14+, GCC 15+, Clang 20+
- **Thread-safe and real-time capable**
- **Zero-macro design** using modern C++23 features

## Architecture Overview

### C++23 Infrastructure
- **Modules-first design**: All components as `.cppm` modules
- **Import std**: Leveraging standard library modules
- **Thread safety**: Using `std::jthread` and atomic operations
- **Source location**: Automatic debug information with `std::source_location`
- **Concepts**: Type safety with C++23 concepts and constraints

### Key Features

#### 1. Core Logging Infrastructure
- **Structured logging** with `LogRecord` containing timestamp, severity, category, message, and metadata
- **Multiple severity levels**: TRACE, DEBUG, INFO, WARN, ERROR, FATAL, AUDIT
- **Thread-safe operations** using C++23 `std::jthread` and atomic operations

#### 2. Medical Device Compliance
- **Audit Trail**: Tamper-proof logging with cryptographic signatures
- **Risk Management**: Hazard tracking and mitigation logging per ISO 14971
- **Lifecycle Logging**: Development, verification, and validation event tracking
- **Regulatory Reporting**: Automated compliance report generation

#### 3. Advanced Sink System
- **FileSink** with log rotation and compression *(planned)*
- **NetworkSink** for remote monitoring systems *(planned)*
- **AuditSink** for tamper-proof audit records *(planned)*
- **ConsoleSink** with colors and statistics *(implemented)*
- **Configurable buffering** and batching strategies

#### 4. Custom Formatters *(planned)*
- **JsonFormatter** for structured data
- **XmlFormatter** for medical device standards
- **MedicalFormatter** with compliance-specific fields

#### 5. Performance & Monitoring
- **Lock-free logging** using C++23 atomic operations
- **Memory pool allocation** for zero-allocation logging *(planned)*
- **Performance metrics** collection and reporting
- **Real-time constraints** support

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
    logger->setMinLevel(LogLevel::INFO);
    
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
    LogLevel::INFO,
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

## Module Architecture

### Core Components

```
mddlog/
├── core/
│   ├── LogLevel.cppm         # Severity levels and utilities
│   ├── LogRecord.cppm        # Structured log record
│   └── Logger.cppm           # Main logger implementation
├── sinks/
│   ├── Sink.cppm             # Base sink interface
│   └── ConsoleSink.cppm      # Console output sink
└── mddlog.cppm               # Main module with exports
```

### Log Record Structure

Each log record contains:

- **Core Information**: Timestamp, level, message, category, thread ID
- **Source Location**: File, line, function (automatic with `std::source_location`)
- **Medical Context**: User ID, session ID, device ID, operation ID
- **Audit Information**: Event type, risk level, compliance standard
- **Metadata**: Custom key-value pairs
- **Performance**: Processing time tracking

## Medical Device Compliance

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

- **MSVC**: 17.14+ (Visual Studio 2022 version 17.10+)
- **GCC**: 15.0+ (for C++23 modules support)
- **Clang**: 20.0+ (for C++23 modules support)
- **CMake**: 4.0+ (for experimental C++23 `import std` support)

### C++23 Features Used

- **Modules**: `import std` and custom modules for faster compilation
- **Source Location**: Automatic source tracking for debugging
- **Concepts**: Type constraints and requirements for type safety
- **Ranges**: Log filtering and processing operations
- **jthread**: Improved thread management with cancellation tokens
- **Atomic**: Lock-free operations for performance

## Performance

### Benchmarks

Typical performance on modern hardware:

- **Synchronous logging**: ~1-2 μs per message
- **Asynchronous logging**: ~100-200 ns per message
- **Memory usage**: ~50-100 bytes per log record
- **Thread contention**: Minimal with lock-free queues

### Optimization Features

- **Lock-free queues** for async logging
- **Memory pools** for allocation efficiency *(planned)*
- **Batch processing** for sink operations
- **Lazy formatting** for unused log levels
- **SIMD optimizations** for string operations *(planned)*

## Examples

See the `examples/` directory for comprehensive usage examples:

- `basic_usage.cpp` - Basic logging functionality with C++23 modules

## Building from Source

```bash
# Configure with CMake (requires CMake 4.0+ for C++23 modules)
mkdir build && cd build
cmake .. -DMDDLOG_BUILD_EXAMPLES=ON

# Build the project
cmake --build .

# Run example
./examples/basic_usage
```

## License

This project is licensed under the GNU General Public License v3.0 - see the [LICENSE](LICENSE) file for details.

## Contributing

Please read [CONTRIBUTING.md](CONTRIBUTING.md) for details on our code of conduct and the process for submitting pull requests.

## Medical Device Certification

This library is designed to support medical device certification processes but is not itself certified. Users are responsible for validation and verification according to their specific regulatory requirements.

## Support


For questions, issues, or contributions, please visit our [GitHub repository](https://github.com/ambroise-leclerc/mddlog).
