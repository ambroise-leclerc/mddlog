import std;
import mddlog.core.record;
import mddlog.core.ring;

int main() {
    using namespace mddlog::core;

    RingLog<2> ring;
    const auto first    = ring.tryWrite({.level         = LogLevel::Audit,
                                         .time          = RawTime::unavailable(),
                                         .message       = "governed audit",
                                         .component     = "consumer",
                                         .operationId   = "operation-1",
                                         .correlationId = "correlation-1"});
    const auto hostTime = std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{123}};
    const auto second   = ring.tryWrite({.level = LogLevel::Warn, .time = RawTime::available(hostTime), .message = "governed warning"});
    const auto full     = ring.tryWrite({.time = RawTime::unavailable(), .message = "full"});
    if (first.admission() != Admission::Written || second.admission() != Admission::Written || !full.refusal().has_value()
        || full.refusal()->reason != RefusalReason::RingFull) {
        return 1;
    }

    const auto view = ring.drain();
    if (view.size() != 2 || view.first().size() != 2 || view.first()[0].level() != LogLevel::Audit || view.first()[0].message() != "governed audit"
        || view.first()[0].component() != "consumer" || view.first()[0].operationId() != "operation-1" || view.first()[0].correlationId() != "correlation-1"
        || view.first()[0].time().availability() != TimeAvailability::Unavailable || view.first()[1].message() != "governed warning"
        || view.first()[1].time().availability() != TimeAvailability::Available || view.first()[1].time().value() != hostTime) {
        return 2;
    }
    if (!ring.acknowledge(view, view.size()) || !ring.drain().empty()) {
        return 3;
    }

    const auto afterReuse = ring.tryWrite({.time = RawTime::unavailable(), .message = "reused slot"});
    const auto nextView   = ring.drain();
    if (afterReuse.admission() != Admission::Written || nextView.size() != 1 || nextView.first()[0].message() != "reused slot"
        || !ring.acknowledge(nextView, 1)) {
        return 4;
    }
    return ring.drain().empty() ? 0 : 5;
}
