/** @brief Generated storage cuts with lost witness replies and an independent confirmed-byte oracle. */
import std;
import speclab;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditlogverifier;

#include "../framework/AuditLogRig.hpp"
#include "../framework/AuditReaderExercise.hpp"

namespace {
using namespace mddlog::adapter;

class LostReplyProvider final : public AnchorProvider {
public:
    [[nodiscard]] AdvanceAnswer advance(const AnchorClaim& claim) override {
        auto answer = inner.advance(claim);
        if (loseNext && std::holds_alternative<AnchorStamp>(answer)) {
            loseNext = false;
            ++lostReplies;
            return ProviderUnavailable{};
        }
        return answer;
    }
    [[nodiscard]] RetireAnswer retire(std::string_view stream, std::uint64_t position) override {
        return inner.retire(stream, position);
    }
    [[nodiscard]] LatestAnswer latest(std::string_view stream) override {
        return inner.latest(stream);
    }
    [[nodiscard]] StreamsAnswer streams() override {
        return inner.streams();
    }
    bool     loseNext    = false;
    unsigned lostReplies = 0;

private:
    InMemoryAnchorProvider inner{"generated/witness"};
};

const speclab::Register generatedRecovery{
    "Generated audit cuts preserve confirmed prefixes despite a lost witness reply",
    "integration",
    [] {
        return speclab::Test("audit-generated-storage-recovery")
            .Then(
                "64 seeds combine open/append/sync cuts, four unconfirmed policies and independent retained observations",
                [] {
                    using Medium = InMemoryStorageMedium;
                    const std::array operations{Medium::Operation::Open, Medium::Operation::Append, Medium::Operation::Sync};
                    const std::array effects{Medium::Effect::CutBefore, Medium::Effect::CutPartial, Medium::Effect::CutAfter};
                    const std::array policies{Medium::Unconfirmed::Dropped, Medium::Unconfirmed::Kept, Medium::Unconfirmed::Half, Medium::Unconfirmed::Erased};
                    constexpr std::string_view stream = "generated/producer";
                    for (unsigned seed = 0; seed < 64; ++seed) {
                        Medium            medium{32};
                        LostReplyProvider provider;
                        StorageConfig     config;
                        config.segmentSize        = 2048;
                        config.segmentCount       = 32;
                        config.maxProducerStreams = 1;
                        config.sync.recordBound   = 1;
                        config.provider           = &provider;
                        config.ledger             = LedgerConfig{.streamId = "generated/ledger/1"};
                        auto made                 = PersistingAuditSink::create(medium, config);
                        mddlog::spec::requireReaderInvariant(made.has_value());
                        auto                sink      = std::move(*made);
                        const std::uint64_t confirmed = 3 + (seed % 5);
                        for (std::uint64_t sequence = 1; sequence <= confirmed; ++sequence)
                            mddlog::spec::requireReaderInvariant(sink->accept(mddlog::spec::auditlog::makeEvent(stream, sequence)));
                        mddlog::spec::requireReaderInvariant(sink->durablePosition(stream) == confirmed);
                        provider.loseNext = true;
                        mddlog::spec::requireReaderInvariant(!sink->advanceAnchor(stream) && provider.lostReplies == 1);
                        RetainedPosition retained;
                        LogVerifier      before{medium, provider, retained};
                        const auto       firstReport = before.verify();
                        const auto*      first       = firstReport.find(stream);
                        mddlog::spec::requireReaderInvariant(first && first->report.anchoredThrough == confirmed);
                        std::map<SegmentRef, std::vector<std::uint8_t>> oracle;
                        const auto                                      inventory = medium.segments();
                        mddlog::spec::requireReaderInvariant(inventory.has_value());
                        for (const auto& info : *inventory) {
                            auto bytes = medium.bytesOf(info.segment);
                            bytes.resize(static_cast<std::size_t>(medium.confirmedOf(info.segment)));
                            oracle.emplace(info.segment, std::move(bytes));
                        }
                        const auto operation = operations.at(seed % operations.size());
                        const auto effect    = effects.at((seed / operations.size()) % effects.size());
                        medium.inject(
                            {.operation = operation, .ordinal = medium.calls(operation) + 1, .effect = effect, .partialBytes = 1 + ((seed * 17) % 128)});
                        for (std::uint64_t sequence = confirmed + 1; sequence <= confirmed + 32 && !medium.poweredOff(); ++sequence)
                            (void)sink->accept(mddlog::spec::auditlog::makeEvent(stream, sequence));
                        mddlog::spec::requireReaderInvariant(medium.poweredOff());
                        const auto durable = sink->durablePosition(stream);
                        sink.reset();
                        medium.restart(policies.at(seed % policies.size()));
                        for (const auto& [ref, prefix] : oracle) {
                            const auto bytes = medium.bytesOf(ref);
                            mddlog::spec::requireReaderInvariant(bytes.size() >= prefix.size()
                                                                 && std::ranges::equal(prefix, std::span{bytes}.first(prefix.size())));
                        }
                        LogVerifier after{medium, provider, retained};
                        const auto  secondReport = after.verify();
                        const auto* second       = secondReport.find(stream);
                        mddlog::spec::requireReaderInvariant(second && second->report.anchoredThrough == confirmed && second->report.lastPresent >= durable);
                        config.ledger        = LedgerConfig{.streamId = "generated/ledger/2"};
                        const auto restarted = PersistingAuditSink::create(medium, config);
                        mddlog::spec::requireReaderInvariant(restarted.has_value());
                        // Recovery may inspect the old stream, but may not recycle its identity.
                        mddlog::spec::requireReaderInvariant(!(*restarted)->accept(mddlog::spec::auditlog::makeEvent(stream, 1)));
                    }
                })
            .Execute();
    }};
}  // namespace
