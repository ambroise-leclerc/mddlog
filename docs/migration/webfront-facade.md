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
adoption remain separate review decisions.

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
