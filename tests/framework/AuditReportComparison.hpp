/** @brief Compare semantic reports independently of resource telemetry. */
#ifndef MDDLOG_TESTS_FRAMEWORK_AUDITREPORTCOMPARISON_HPP
#define MDDLOG_TESTS_FRAMEWORK_AUDITREPORTCOMPARISON_HPP
namespace mddlog::spec {
inline void compareAuditReports(const adapter::LogReport& reference, const adapter::LogReport& chunked) {
    const auto require = [](bool holds) {
        if (!holds)
            throw std::runtime_error("chunked audit report differs from reference");
    };
    const auto reportKey = [](const adapter::StreamReport& report) {
        return std::tie(report.streamId,
                        report.verdict,
                        report.cause,
                        report.chainFinding,
                        report.failedAt,
                        report.unknownLayoutVersion,
                        report.firstRetained,
                        report.lastPresent,
                        report.anchoredThrough,
                        report.unanchoredFrom,
                        report.age,
                        report.retained);
    };
    const auto compare = [&](const adapter::StreamReport& first, const adapter::StreamReport& second) {
        require(reportKey(first) == reportKey(second));
        require(first.anchor.has_value() == second.anchor.has_value());
        if (first.anchor && second.anchor)
            require(std::tie(first.anchor->providerId, first.anchor->streamId, first.anchor->position, first.anchor->digest, first.anchor->counter)
                    == std::tie(second.anchor->providerId, second.anchor->streamId, second.anchor->position, second.anchor->digest, second.anchor->counter));
    };
    require(reference.resourceIssue == adapter::AuditResourceIssue::None && chunked.resourceIssue == adapter::AuditResourceIssue::None);
    require(reference.mediumUnreadable == chunked.mediumUnreadable && reference.streams.size() == chunked.streams.size()
            && reference.unlisted.size() == chunked.unlisted.size() && reference.notes.size() == chunked.notes.size());
    for (std::size_t i = 0; i < reference.streams.size(); ++i) {
        const auto& first  = reference.streams.at(i);
        const auto& second = chunked.streams.at(i);
        compare(first.report, second.report);
        require(first.ledger == second.ledger && first.disposition == second.disposition && first.trailing.has_value() == second.trailing.has_value());
        if (first.trailing && second.trailing)
            require(std::tie(first.trailing->segmentIndex, first.trailing->offset, first.trailing->length)
                    == std::tie(second.trailing->segmentIndex, second.trailing->offset, second.trailing->length));
    }
    for (std::size_t i = 0; i < reference.unlisted.size(); ++i)
        compare(reference.unlisted.at(i), chunked.unlisted.at(i));
    for (std::size_t i = 0; i < reference.notes.size(); ++i) {
        const auto& first  = reference.notes.at(i);
        const auto& second = chunked.notes.at(i);
        require(std::tie(first.kind, first.stream, first.other, first.position, first.secondPosition, first.ledger, first.ledgerPosition)
                == std::tie(second.kind, second.stream, second.other, second.position, second.secondPosition, second.ledger, second.ledgerPosition));
    }
}
}  // namespace mddlog::spec
#endif
