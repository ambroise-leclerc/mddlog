# Host civil time quality and monotonic audit time (#154, #155)

Unreleased: `RawTime::doubtful(value)` retains UTC nanoseconds while marking them unreliable.
Only the host detects clock jumps and decides when a subsequent time is available again. No
factory reads a clock or checks plausibility; neither unavailable nor doubtful time refuses an
event. `RawTime::value()` is meaningful for Available and Doubtful.

Consumers that test `availability() == TimeAvailability::Available` still select reliable civil
time only. Review every `!= TimeAvailability::Unavailable` test: it now includes doubtful time.
Diagnostic `LogRecord::timeAvailable` means a numeric timestamp exists; the new `timeDoubtful`
flag distinguishes its quality, and `getFormattedTimestamp()` appends ` [doubtful]`. Unavailable
time still renders `unavailable`. Age verification continues to require Available for both clocks;
a doubtful verification or acceptance time yields Unknown age.

Set `AuditInput::monotonicTime` or `AuditEventOptions::monotonicTime` to an optional
`std::chrono::nanoseconds` count since producer startup. The new aggregate members are appended,
so existing field order remains valid. `AuditEvent::monotonicTime()` returns the exact admitted
value, independently of detail truncation. Absence is distinct from zero. Signed counts are
preserved without validation, as for civil time: the host supplies and owns elapsed-time policy.
Values are comparable only within one producer and boot/stream instance. Keep unique stream IDs
across boots; sequence remains the strict per-stream ordering key, and no global order is inferred.

Canonical writers now emit v2 (civil-time tag 2 for doubt, optional startup nanoseconds after the
truncation flag). Readers still verify v1 bytes without re-encoding; v1 has no monotonic value.
Start a fresh stream for v2; a version change inside one stream is inconsistent. Maximum frames
increase from 813 to 822 bytes, so recheck segment size and reserved-segment budgets.

Evidence and JSON projection writers also emit v2. Evidence v1 remains readable; projection v2
contains explicit time quality and nullable startup nanoseconds. Witness protocol/state v2
preserves doubtful acceptance/retirement times. Upgrade live witness client and service together;
the authority reads existing v1 state and writes v2 on its next durable update. Old binaries refuse
unknown versions. Keep a copy of old state if a deployment needs rollback to an old binary.

See [canonical contract](../adr/ADR-004-audit-persistence-and-tamper-evidence.md#82a-canonical-version-2-154-155-unreleased)
and [export formats](../audit-export-format.md). These changes do not qualify a deployment or
validate its clocks.
