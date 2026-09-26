# Migrating to the governed core

The governed core is a separate C++23 module library. Use it when a producer needs bounded,
owned records and a fixed-capacity SPSC ring without `SimpleLogger` or sinks. The existing
allocating logger and `import mddlog;` umbrella remain available through the full target.
Neither path is certified or production-validated medical-device software.

## Direct imports and build targets

The adapter modules were moved out of `mddlog.core.*`. There are no compatibility shim modules:

| Former direct import | Current direct import |
| --- | --- |
| `mddlog.core.logger` | `mddlog.adapter.logger` |
| `mddlog.core.logrecord` | `mddlog.adapter.logrecord` |
| `mddlog.core.loglevel` | `mddlog.core.loglevel` (unchanged) |

`SimpleLogger` and the allocating `LogRecord` retain their existing C++ names. The moved module
names matter to code with direct imports; applications using only `import mddlog;` need no import
change. Sink imports remain under `mddlog.sinks.*`. The governed `GovernedRecord` is a different
value type, not a drop-in replacement for the allocating `LogRecord`.

For an in-tree `add_subdirectory(mddlog)` integration or an installed
`find_package(mddlog CONFIG REQUIRED)` integration, link `mddlog::core` if the target uses only
governed modules. Link `mddlog::mddlog` for `SimpleLogger`, the umbrella, adapters or sinks; the
full target depends on the core target. Do not link the full target to a core-only consumer just
to make imports work. A target that links `mddlog::mddlog` and also directly imports a
`mddlog.core.*` module (for example to create a `RingLog` for `RingSinkAdapter`) must link
`mddlog::core` explicitly as well: with GCC and CMake 4.1, modules reached only through a
transitive link are not placed in the importing file's module mapper, and the import fails with
"unknown compiled module interface".

```cmake
find_package(mddlog CONFIG REQUIRED)
add_executable(consumer CoreConsumer.cpp)
target_link_libraries(consumer PRIVATE mddlog::core)
```

The consumer may directly `import mddlog.core.record;` and `import mddlog.core.ring;`.
`mddlog.core.record` re-exports its inline-string, log-level and write-result dependencies. The
core-only program in [`tests/consumer/CoreConsumer.cpp`](../../tests/consumer/CoreConsumer.cpp)
is built and run both in-tree and from an installed package.

## Adapting an allocating producer

`RecordInput` contains a level, host-supplied `RawTime`, source location, message, component,
operation ID and correlation ID. Its default level is `Info`, and its default source location
captures the aggregate-initialization site; `.time` must be supplied. Use
`RawTime::available(hostUtcNanoseconds)` when the host has a UTC timestamp, or
`RawTime::unavailable()` otherwise. The core never consults a clock.
Time formatting, stream conversion and output stay in adapters or sinks, not the governed record.

`GovernedRecord` owns its fixed-capacity strings. Message capacity is 160 bytes; overlong UTF-8
messages are shortened at a code-point boundary and `WriteResult::truncated().message` reports
that change. Component and operation IDs each have a 32-byte capacity; correlation ID has a
40-byte capacity. An overlong identifier is refused, never silently truncated. Check
`WriteResult::admission()` before using `refusal()`; the latter is populated only for a refused
write. `RingLog<N>::tryWrite()` can also refuse with `RingFull`, whose count is available via
`refusalCount()` as a saturation signal.

One producer and one consumer may use each `RingLog<N>` concurrently. The consumer reads or copies
both spans returned by `drain()` before calling `acknowledge(view, count)`; check its boolean
result. A positive acknowledgement invalidates the view, allowing its slots to be reused. Use
separate rings for multiple producers; no cross-ring ordering is guaranteed. The full adapter's
`RingSinkAdapter` can copy these records into sinks when sink output is needed, but it is not part
of the governed core.
