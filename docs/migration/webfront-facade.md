# WebFront diagnostic facade (#69)

`import mddlog.adapter.textlogger;` provides `mddlog::adapter::TextLogger` in the
`mddlog::mddlog` target. Its atomic per-group flags start disabled; `set()` and `is()`
operate independently, so Warn can be disabled while Error remains enabled. Trace shares
Debug's mask and renders as `D`; Fatal shares Error's mask and renders as `E`. Invalid
severity values are disabled. `disableAll()` masks every group.
Atomicity applies to each group separately. `disableAll()` and the facade's `setLogLevel()`
update groups in sequence, with no atomic transition across groups. Concurrent `is()` or
write calls may therefore observe a partially applied change.

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
adoption remain separate review decisions. See the optional WebFront integration below.

The header retains `webfront::log::debug`, `info`, `warn`, `error`, `infoHex`, `set`, `is`,
`setLogLevel`, `addSinks`, and `removeSinks`. Mapping is an explicit switch, never a numeric
cast. `Disabled` masks all groups. Only `debug` captures the call site's source location.
Disabled calls skip formatting. `infoHex` accepts contiguous numeric/byte buffers and retains
the original dump spacing, offsets and separate second write. The ASCII column prints only
bytes `0x20` through `0x7E`; all others, including DEL (`0x7F`) and `0x80` through `0xFF`,
render as `.`, independently of whether plain `char` is signed on the platform.

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
the actual debug caller file/line even on LLVM, formatting guards, dump output for every
possible byte value, independent multi-registration removal and compile-time audit exclusion.
Compilation, linking and test
execution must each succeed; configuration alone is not verification.

## Emission context (#70)

The reference bridge now supplies a thread-local `ContextScope` and a thread-local
`setRecordWriter()` binding. Set a scope explicitly at each request, reply, exception,
close, and asynchronous completion entry point; scopes nest and restore the previous
context. Context does not automatically migrate between threads. The scope borrows its
strings until exit; the writer copies them into the ring during emission. No connection
pointer reaches the drain. With no scope all three identifiers are empty. For connection
logs outside an outstanding call, supply the component and link with `None` and an empty
call id; operationId is empty. Use a direction and a nonempty call id for call logs.

The encoding is byte-exact (no text parsing):

| Envelope field | Value | Maximum bytes |
|---|---|---|
| `component` | component supplied by the host | 32 |
| `correlationId` | decimal WebLinkId supplied by the host | 40 |
| `operationId` | `cpp-js:` + decimal CallId, or `js-cpp:` + decimal CallId | 32 |

Both prefixes consume seven bytes, leaving 25 bytes for a call id. The normal uint16
identifiers fit. Values exceeding any capacity are refused in component, operation,
correlation order, before the writer is called, even if the ring is full. The message
capacity is 160 bytes; the ring truncates it at a UTF-8 boundary and owns its truncation
flag. The writer's `DiagnosticRecord.message` borrows the original formatted message
so that `RingLog::tryWrite()` performs that truncation itself. The other views refer to
validated context and must also be copied before the writer returns. The snapshot
includes emission time, level and location. Only diagnostic types enter this API.

A host module-consuming TU binds each producer thread to its own ring, for example:

```cpp
log::setRecordWriter([&ring](const log::DiagnosticRecord& value) {
    // Map all four WebFront levels explicitly, as Logger.cpp does.
    auto result = ring.tryWrite({.level = mapDiagnosticLevel(value.level),
        .time = mddlog::core::RawTime::available(value.time),
        .location = value.location, .message = value.message,
        .component = value.component, .operationId = value.operationId,
        .correlationId = value.correlationId});
    return result.admission() == mddlog::core::Admission::Written;
});
{
    log::ContextScope scope({"jsFunction", "2", log::CallDirection::CppToJs, "1"});
    const auto outcome = log::tryWrite(log::Info, "request sent");
    // Handle IdentifierTooLong or RingFull locally; do not log refusals recursively.
}
log::setRecordWriter({}); // before destroying the producer's ring
```

`tryWrite()` returns `WriteOutcome`; existing void calls expose their latest outcome
through `lastWriteOutcome()` on the same thread. Enabled legacy text delivery is independent
of structured refusal: a full ring or an overlong identifier does not suppress console/file
callbacks. Disabled calls skip formatting and emission; their legacy wrapper does not replace
the previous outcome. `tryWrite()` itself returns Filtered. RingFull also increments the
ring's saturation counter.

`infoHex(text, bytes, location)` defaults the location at the caller and forwards it to both
structured records (message, then dump). It returns `HexWriteOutcome`, with separate `message`
and `dump` outcomes. Existing callers may ignore the return value. Two available ring slots
are required for complete structured admission. If only one fits, the message remains admitted
and `result.dump.status` is RingFull, without rollback; legacy callbacks still receive both
complete text writes. The dump remains one 160-byte governed message, so larger dumps are
shortened with `result.dump.messageTruncated` and the ring record's truncation flag set.
`lastWriteOutcome()` summarizes the first refusal, or the dump result when the message was
admitted, and ORs the truncation flags of admitted records. Check the two returned outcomes
when partial admission matters. Neither identifiers nor missing dump bytes are reconstructed
from legacy text by the consumer.

A throwing writer propagates its exception, leaves `lastWriteOutcome()` at the last
completed capture and prevents legacy text delivery for that attempt. The host must guarantee
writer removal before ring destruction on exceptional exits as well as normal teardown
(for example with a host-owned scope guard). `ContextScope` restores context during
unwinding but does not manage the independent writer binding.
With no writer the legacy text output remains available. Binding a writer adds the
structured lane; synchronous text callbacks still run, so browser transports must be
registered on `TransportConsumer`, not as facade text callbacks. The facade's formatting,
clock read, callback binding and short identifier concatenation remain adapter operations
and may allocate. The ring producer itself retains ADR-001's bounded contract.

Register rings with #68's `TransportConsumer::addRing()` before its single consumer starts.
Each ring has exactly one producer and one consumer; multiple producer threads need
separate rings. Consumer-thread logging uses `addConsumerRing()` to suppress transport
feedback. Alternatively `RingSinkAdapter` copies the same identifiers to diagnostic sinks;
do not register one ring with both consumers. Every transport receives the captured fields
in its owning `LogRecord`, even after the original connection has been destroyed.
Source location and the legacy text/hex rendering remain compatible with #69.

The triplet `(correlationId, direction, callId)` identifies a call only while outstanding.
Both WebLinkId and CallId counters wrap and are reused; the two directions allocate CallId
independently. For correlation beyond this window, the host must attach a process/session
and link-generation discriminator in its external envelope, or encode a session-qualified
link id within the 40-byte correlation capacity (refusing longer values). The component is
not part of call identity: the second request for link 2 remains the same call when its
component changes from `jsFunction` to `weblink` on disconnect. ADR-003 remains Proposed;
this reference implementation does not change its review status or deploy WebFront adoption.

`WebFrontContextSpec.cpp` reproduces all seven events of Decision 6, destroys borrowed
connection strings before draining, compares delivery to two transports, and checks exact
field capacities, identifier refusals, UTF-8 truncation, nested scopes, out-of-call context,
observable ring saturation, hex caller locations, explicit dump truncation and independent
legacy text delivery on structured refusal. The original WebFront suite and #69 regressions still run.


## Optional WebFront integration (#71)

WebFront revision `b2c2dd6f2f03b826337c5d0f2a06c2445cda2f18` adopts the facade behind
`WEBFRONT_USE_MDDLOG=OFF` by default. WebFront owns its facade declarations, ordinary
header and module-consuming `logging/Logger.cpp`; mddlog still installs only module
libraries. Enabling the option links WebFront's compiled adapter through its `WebFront`
target. Ordinary consumers keep including `tooling/Logger.hpp` and import no modules.
`WEBFRONT_MDDLOG_SOURCE_DIR` selects a source checkout; leaving it empty selects
`find_package(mddlog CONFIG REQUIRED)` and the supplied installation prefix.

With the option off WebFront remains standalone/header-only with CMake 3.31. Its normal
CI pins CMake 3.31.10. The enabled path selects the experimental gate before compiler
discovery, requires qualified CMake 4.0–4.3 and a Ninja generator, and enforces mddlog's
compiler/platform restrictions. Apple Silicon requires upstream LLVM/Clang 21.1.8,
matching llvm-ar/llvm-ranlib, libc++ and CMake 4.3.1 exactly; AppleClang is rejected.
See [WebFront's integration guide](https://github.com/ambroise-leclerc/WebFront/blob/b2c2dd6f2f03b826337c5d0f2a06c2445cda2f18/docs/mddlog-integration.md)
for the complete matrix, linkage, runtime and registration ownership rules.

The opt-in path preserves the toolchain's MSVC runtime choice. Source integration scopes
mddlog's dependency options and prepends its own CMake helpers, avoiding collisions with
WebFront helpers of the same names. In particular mddlog's Clang BMI warning policy is
needed when CMake recompiles installed module interfaces. The enabled POSIX path uses
`Threads::Threads`, avoiding a late directory-wide `-pthread` that would disagree with
Clang's std BMI. Toolchains needing explicit thread flags must initialize them consistently.

With `MDDLOG_BUILD_TESTS=ON`, enable `MDDLOG_BUILD_WEBFRONT_INTEGRATION_TESTS`
(default `OFF`) to fetch the pinned integration revision and register two CTest tests.
Leaving it off avoids this additional network/cache dependency; the historical test
suite still needs its existing SpecLab, Catch2 and WebFront baseline dependencies.
The two tests configure the **actual WebFront project**, compile and link the adapter,
compile/link its header consumers, and run the full CEF-off suite:

```bash
cmake --preset ninja-clang -DMDDLOG_BUILD_WEBFRONT_INTEGRATION_TESTS=ON
cmake --build --preset ninja-clang
ctest --test-dir build-clang -R '^webfront.integration.' --output-on-failure
```

`webfront.integration.source` builds against this mddlog source tree.
`webfront.integration.installed` first removes its scratch prefix and installs the current
build, then uses its package. Both tests clear their consumer build directory when the
forwarded toolchain/configuration inputs change, including when an optional input is
removed. Unchanged inputs retain incremental builds. Each reports configuration, adapter
compilation/static linkage, consumer compilation/executable linkage and execution as separate stages. Both run the
unchanged original `LoggerTests.cpp` and the WebFront-owned facade regressions. They
are explicitly enabled on the four supported release CI legs, with the compiler, std manifest,
archive tools, sysroot, flags, configuration and MSVC runtime forwarded. The historical
#66/#69 reference tests remain registered independently. No new mddlog module or installed
facade header is introduced.

Local verification on Linux used Clang 21.1.8/libc++, CMake 4.2.3/Ninja for the enabled
source and installed paths (68 WebFront tests each), and CMake 3.31.10/Unix Makefiles
for the default standalone path (63 tests). These local results do not establish results
on other CI platforms. Configuration with CMake 3.31 and the option on fails explicitly.
ADR-003 remains **Proposed**; option adoption does not accept the ADR or install the
structured producer/context/transport bindings described in #70.

## WebFront adoption (#72)

WebFront revision `35141a5e5874f8f66464ebbbcaf7aed90539c054` binds the browser sink,
emission context and lifecycle described above. `webfront.integration.*` now builds
and tests that revision. ADR-003 remains **Proposed**; adoption does not accept it.

- **Bounded browser lane.** With `WEBFRONT_USE_MDDLOG=ON`, `log::addTransport()` registers
  the browser sink on `TransportConsumer`, not as a synchronous `TextLogger` callback. The
  first registration starts a WebFront-owned consumer thread. All producer rings are
  registered before it starts, as `addRing()` requires: a fixed pool of 32 rings of
  128 records. A producer thread claims one free ring on its first emission while a
  transport is attached and returns it when it exits. The pool mutex orders each
  hand-over, so each ring has one producer at a time and one consumer. An exhausted pool
  returns `RingUnavailable`; a full ring returns `RingFull`; neither waits.
- **Bounded browser output.** The rings bound producers, not the WebSocket's write queue the
  consumer feeds. Diagnostic frames use WebFront's `WebSocket::tryWrite`, refused once 64
  frames await the network; refusals are dropped without logging and counted by
  `WebLink::droppedLogFrames()` and `TransportHealth::overflows`. Application frames are
  never dropped. A stalled browser (one write never completing, 10,240 diagnostics) leaves
  64 frames queued and 10,176 counted refusals.
- **Consumer-thread diagnostics.** The consumer thread's records go to a ring registered with
  `addConsumerRing()`. Those emitted during dispatch are acknowledged without delivery and
  counted in `reentrantRecords`.
- **Failure ordering.** WebFront's `WebSocket::onWriteError` runs before the write-error
  diagnostic, including after `stop()`. `WebLink` calls `reportFailure` through the facade,
  then the WebSocket logs. A normal close calls `removeTransport`. No
  `WebLinkEvent::Code::closed` is emitted; the destructor removes the transport before its
  own diagnostic. Detachment happens once and the link never attaches again.
- **Context.** `WebLink` installs `ContextScope` for received messages, close and
  destruction (`weblink`), calls from JavaScript (`cppFunction`, `js-cpp:<CallId>`) and
  returns of C++ calls (`jsFunction`, `cpp-js:<CallId>`). Transports receive the owned
  fields of #70 after the link has gone.
- **Registrations.** `main` in `HelloWorld.cpp` and `JasmineTest.cpp` keeps and removes its
  console handle. `LoggerTests.cpp` already did and stays byte-for-byte unchanged.
- **Audit.** Neither the facade nor the transport lane accepts audit types.
  `TransportConsumerSpec.cpp` checks at compile time that `TransportConsumer` rejects
  an `AuditRing`, an `AuditSink` and an audit-event callback.

`MddlogTransportTests.cpp` in WebFront covers saturation while the consumer is blocked,
synchronous throws, reentrant logging, asynchronous failure reports, quiescent removal
during concurrent emission, the seven events, and WebLink disconnection, write failures
and destruction before the drain. Its results, including sanitizer runs, are reported
in the adoption pull requests.
