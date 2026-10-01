# WebFront diagnostic facade (#69)

`import mddlog.adapter.textlogger;` provides `mddlog::adapter::TextLogger` in the
`mddlog::mddlog` target. Its atomic per-group flags start disabled; `set()` and `is()`
operate independently, so Warn can be disabled while Error remains enabled. Trace shares
Debug's mask and renders as `D`; Fatal shares Error's mask and renders as `E`. Invalid
severity values are disabled. `disableAll()` masks every group.

`write(level, text)` renders `[X] HH:MM:SS | text`. Passing a `source_location` adds
`file:line |` between the time and message, with the basename padded to 16 columns and
the line padded to 4. The timestamp uses `system_clock` and retains the standard library's
fractional-second precision. `writeDump()` emits a rendered message followed by a separate,
unprefixed dump, under one enablement decision. Rendering and callbacks are synchronous and
allocating; browser transport buffering and failure handling remain separate (#68/#70).

Callbacks are registered through #67's `SinkRegistry<void(string_view)>`. Each callback has
its own handle. Removal outside a callback waits for quiescence; self-removal and detected
mutual cross-removal follow the registry's deferred-removal contract. Handles are explicit
registrations, not RAII subscriptions: retain them and remove them before destroying captured
objects. A callback must copy any text it retains after returning.

## Reference bridge and ownership

ADR-003 Decision 3 assigns the module-free facade header and the module-consuming TU to
WebFront, behind an off-by-default WebFront option. This repository implements and validates
that boundary with `tests/webfront/tooling/Logger.hpp` and `tests/webfront/Logger.cpp`.
`LoggerApi.hpp` holds their shared declarations: the ordinary header includes standard headers,
while the module-consuming TU includes those declarations after `import std`, avoiding duplicate
standard-library declarations across the two mechanisms.
These are a reference bridge for adoption, built only with `MDDLOG_BUILD_TESTS=ON`;
they are not installed or linked into the production library. ADR acceptance and WebFront
adoption remain separate review decisions.

The header retains `webfront::log::debug`, `info`, `warn`, `error`, `infoHex`, `set`, `is`,
`setLogLevel`, `addSinks`, and `removeSinks`. Mapping is an explicit switch, never a numeric
cast. `Disabled` masks all groups. Only `debug` captures the call site's source location.
Disabled calls skip formatting. `infoHex` accepts contiguous numeric/byte buffers and retains
the original dump spacing, offsets, ASCII column and separate second write.

One callback returns one opaque handle. Multiple callbacks return an array of handles:

```cpp
auto [first, second] = webfront::log::addSinks(callbackA, callbackB);
webfront::log::removeSinks(first, second);
```

Neither the facade nor `TextLogger` has an audit admission or audit forwarding API.
Format arguments must satisfy `std::formattable`; `AuditEvent` is rejected as a message,
format argument, dump buffer, level, registration or removal argument. An `AuditSink`
cannot be registered as a text callback. Deliberately serializing audit data to text in
application code is outside this typed boundary.

## Reproduce the acceptance checks

With tests enabled, CMake downloads WebFront at
`7be626ccfbb50524c9c03296e6c84555ea7d2c7c` (the #66 baseline) without configuring its
project, and Catch2 at `56809e5282f104c5c8b570e7c2996cdc352d94f1` (v3.8.1).
`LoggerTests.cpp` is copied byte-for-byte and compiled against the reference header;
none of its assertions, sections or LLVM guards are changed.

```bash
cmake --preset ninja-clang
cmake --build --preset ninja-clang --parallel
ctest --test-dir build-clang -R 'webfront|WebFront|TextLogger' --output-on-failure
```

`webfront.baseline.LoggerTests` runs the original Catch2 suite. `TextLoggerSpec.cpp`
additionally checks initial disablement, independent masks, all six severity mappings,
the actual debug caller file/line even on LLVM, formatting guards, dump output, independent
multi-registration removal and compile-time audit exclusion. Compilation, linking and test
execution must each succeed; configuration alone is not verification.
