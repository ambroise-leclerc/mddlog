# Transport consumer rings and reentrant logging

`TransportConsumer` consumes bounded `RingLog`s on one host-owned consumer thread.
Register rings before starting consumption or concurrent health observation:

- `addRing(ring)` registers a ring owned by an independent producer thread.
- `addConsumerRing(ring)` registers a ring whose sole producer is the consumer thread,
  including logs emitted synchronously by transport callbacks.

Each ring still has exactly one producer and one consumer. Never write a producer-owned ring
from a transport callback. Both kinds of ring must outlive the consumer.

Previously, a callback log could be delivered on the next `drainOnce()`. If every transport
write logged again, this created unlimited feedback from one external record. Consumer-owned
rings are now snapshotted with the other rings before dispatch. Records written into them during
dispatch are acknowledged without delivery to any transport. `healthSnapshot().reentrantRecords`
counts these suppressed records without invoking a sink. Records written outside dispatch are
still delivered. This intentionally suppresses callback diagnostics for all transports, rather
than retaining sink-specific provenance in governed records.

A nested `drainOnce()` on the consumer thread returns zero. Concurrent calls from multiple
consumer threads remain unsupported. No suppression reads or discards independently produced
records, and saturation remains an immediate refusal counted by `ringRefusals`.

Use `removeTransport(handle)` on normal disconnection. Use `reportFailure(handle)` for a
transport error, before logging the error elsewhere. Both retire the sink through the registry's
quiescent removal contract; self-removal is deferred until the callback returns. Normal removal
releases the self-handle lookup cell without incrementing failure counters.

This implements the transport portion of proposed ADR-003; it does not accept the ADR or provide
WebFront's facade from issue #69.
