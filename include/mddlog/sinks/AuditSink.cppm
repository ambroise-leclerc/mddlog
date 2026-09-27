/** @brief Separate sink contract for runtime audit events. */

export module mddlog.sinks.auditsink;

import std;
export import mddlog.core.auditevent;

export namespace mddlog::sinks {

/**
 * @brief Consumer of audit events, independent of diagnostic severity and sink filtering.
 *
 * Returning true means the sink took responsibility for an event in memory. It does not mean
 * durable storage. Returning false or throwing leaves the source ring event unacknowledged.
 * Implementations must tolerate a retry of the same (streamId, sequence) after an uncertain
 * failure, and must copy an event if they retain it beyond accept().
 */
class AuditSink {
public:
    AuditSink()                            = default;
    AuditSink(const AuditSink&)            = delete;
    AuditSink& operator=(const AuditSink&) = delete;
    AuditSink(AuditSink&&)                 = delete;
    AuditSink& operator=(AuditSink&&)      = delete;
    virtual ~AuditSink()                   = default;

    [[nodiscard]] virtual bool accept(const core::AuditEvent& event) = 0;

    [[nodiscard]] bool isEnabled() const noexcept {
        return enabled.load();
    }
    void setEnabled(bool value) noexcept {
        enabled.store(value);
    }

private:
    std::atomic<bool> enabled{true};
};

using AuditSinkPtr = std::shared_ptr<AuditSink>;

}  // namespace mddlog::sinks
