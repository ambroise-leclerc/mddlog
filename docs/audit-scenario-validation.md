# ADR-002 scenario validation for #59

ADR-002 remains **Proposed**. This report evaluates the implemented audit core and the
current call sites; it does not record maintainer acceptance. `tests/spec/AuditScenarioSpec.cpp`
is executable evidence for the bounded record and memory admission contract.

## Category and identifier review

| Scenario and evidence | Category | Stable action and references | Assessment |
| --- | --- | --- | --- |
| MduX `ActionTrace` for `SystemEvent::TriggerHalt` on `emergency-halt`, `REQ-EM-003` | `RiskControl` | `TriggerHalt`, `REQ-EM-003`; hazard ID only if the host has one | Fits a request, confirmation, and result. MduX describes the trace as a critical press the host is about to act on; the host owns execution. |
| `examples/basic_usage.cpp` and `examples/simple_usage.cpp`: `CONFIG_CHANGE`, `admin456`, `device_789`, `MEDIUM` | `Configuration` | `CONFIG_CHANGE`; setting or configuration object should replace a device-only target where known | `MEDIUM` is a risk *level*, not a risk-control reference. Leave `riskRef` empty unless the caller supplies a stable hazard ID. |
| `examples/basic_usage.cpp` and `tests/spec/AuditPolicySpec.cpp`: access to patient data, `DATA_ACCESS`, `user123` | `Access` | `DATA_ACCESS`; identify the accessed object without putting patient data into an identifier | Fits, but the examples provide only a device ID as target; a real producer must define the actual protected object. |
| Device start, stop, or maintenance state transition | `Lifecycle` | Host-defined stable transition ID | Category is plausible; no runtime audit call site in this repository validates its vocabulary. |
| Operator acknowledgement of an alarm | `Operator` | Host-defined acknowledgement ID, alarm target | Category is plausible; no runtime call site in this repository validates its vocabulary. A confirmation phase of `TriggerHalt` can remain `RiskControl`; category need not change between phases. |

At the time of #59, the existing `logAudit()` calls were still on the diagnostic path.
The #58 migration removed those overloads. Their
`message` maps to bounded `detail`, `eventType` to `action`, `userId` to `actor`, and
`deviceId` to `target` only when the device is the object acted on. A device ID may also
be part of the host-issued stream identity. `riskLevel` cannot automatically become a
hazard reference. The old default `IEC_62304` is not a per-event requirement reference.
Free text with spaces, non-ASCII bytes, or excess length cannot be copied into an
identifier: choose a stable code and place the description in `detail`. The admission
result identifies the refused field; only `detail` may shorten, with a flag. #58 must
apply this mapping to the public logger APIs before those paths satisfy ADR-002.

The initial grammar is ASCII letters, digits, `_`, `.`, `:`, `/`, `-` for every
identifier. `action`, `target`, and `streamId` are required. Exact limits in bytes are
`action` 64, `actor` 64, `target` 96, `requirementRef` 64, `riskRef` 64,
`correlationId` 117, `streamId` 96, and `detail` 160. The 117-byte correlation bound
supports `<sourceStreamId>:<decimalSourceSequence>` even for a 96-byte source identity
and `UINT64_MAX`. These capacities fit the observed repository examples, but external
deployment call sites have not been surveyed; acceptance needs that review.

## ActionTrace conversion and event order

The [pinned MduX `ActionTrace` declaration](https://github.com/ambroise-leclerc/MduX/blob/d972d77bc5cefdbe105ad7933ee61746fb5eb45b/include/mdux/medui/Input.cppm#L834-L853)
defines a trace of a critical press and explicitly assigns device behavior and audit
persistence to the host. At the host boundary, copy `ActionTrace.nodeId` to `target`, map the closed
`SystemEvent` to a stable `action`, copy `requirement` to `requirementRef`, and copy
`sequence` to `sourceSequence`. The host supplies `actor`, `riskRef`, `time`, and a
source stream identity. It constructs `correlationId` from that identity and source
sequence, such as `device7:boot9:ui1:41`; the bare `41` can collide with another
source. No MduX import is needed in mddlog.

For a critical action, record `Requested` when the trace is produced. Record
`Confirmed` only after a real acknowledgement, when the workflow requires one. After
the host acts, record exactly the observed outcome as `Executed` or `Failed`. Do not
derive an outcome from the request. Every admitted event receives its own audit
sequence; all events of the action carry the same `sourceSequence` and correlation.
The interleaved test records requests 41 and 42, confirmation and execution of 41,
then failure of 42, yielding audit sequences 1 through 5 without a collision.

Within a stream, sequence determines emission order even when times repeat, move
backwards, or are unavailable. Independent producers can each emit sequence 1; their
stream IDs distinguish the pairs. A recreated producer and a new boot session must
receive new stream IDs when counters restart. There is no global order between streams.
The tests exercise these cases with distinct identities and correlations. The library
validates identity syntax, but uniqueness remains a host responsibility.

## Delivery scope and outstanding review

The audit ring refuses new events when full, retains admitted records until
acknowledgement, and reports refusal without consuming a sequence. #57 adds an audit
sink, hand-off status, and health counters; acceptance by the sink means only that it
has taken responsibility in memory. A failed hand-off leaves the event queued for
retry. Neither admission nor hand-off survives power loss; durable confirmation and
tamper evidence remain in #11.

Implementation evidence: #56 supplies the bounded record and ring; #57 supplies
consumption and health. This issue supplies scenario and boundary tests plus the
correlation capacity correction. #58 subsequently migrated `logAudit()` and removed
`LogLevel::Audit`. ADR-002 acceptance remains a separate maintainer decision, and
#9 can close only after its remaining implementation, validation, and review criteria
are satisfied. The tests establish the behavior of the exercised implementation,
not field suitability for deployments or a production validation claim.
