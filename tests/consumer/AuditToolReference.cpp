/** @brief Real-file reference journal and direct library oracle for the audit CLI. */
#include <unistd.h>
import std;
import mddlog.core.auditevent;
import mddlog.adapter.auditstore;
import mddlog.adapter.auditprojection;
import mddlog.adapter.filestoragemedium;
import mddlog.adapter.fileanchorauthority;
import mddlog.adapter.unixanchorprovider;

int main(int argc, char** argv) {
    using namespace mddlog::adapter;
    try {
        const std::span args(argv, static_cast<std::size_t>(argc));
        if (args.size() != 4)
            return 2;
        const std::string mode(args[1]);
        if (mode == "serve") {
            const auto authority = FileAnchorAuthority::open({.directory = args[3], .providerId = "cli-witness"});
            if (!authority)
                return 2;
            const auto service = UnixWitnessService::create({.socketPath = args[2], .providerId = "cli-witness", .readers = {::getuid()}, .permissions = {}},
                                                            **authority);
            if (!service)
                return 2;
            for (;;) {
                if (!(*service)->serveOnce())
                    return 2;
            }
        }
        FileStorageConfig files{.directory       = args[2],
                                .maxSegments     = 32,
                                .maxSegmentBytes = 8192,
                                .maxReadBytes    = 65536,
                                .access          = mode == "create" ? FileAccess::ReadWrite : FileAccess::ReadOnly,
                                .durability      = FileDurability::QualifiedFsync};
        const auto        medium = FileStorageMedium::create(files);
        if (!medium)
            return 2;
        WitnessStoreConfig witness{.directory = args[3], .providerId = "cli-witness"};
        if (mode == "create" && !FileAnchorAuthority::initialize(witness))
            return 2;
        const auto authority = FileAnchorAuthority::open(witness);
        if (!authority)
            return 2;
        if (mode == "create") {
            StorageConfig config;
            config.segmentSize        = 8192;
            config.segmentCount       = 32;
            config.maxProducerStreams = 2;
            config.sync.recordBound   = 1;
            config.provider           = authority->get();
            config.ledger             = LedgerConfig{.streamId = "cli/ledger", .time = {}};
            const auto sink           = PersistingAuditSink::create(**medium, config);
            if (!sink)
                return 2;
            for (const std::string_view stream : {"cli/alpha", "cli/beta"}) {
                for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
                    mddlog::core::AuditEvent event;
                    if (!event
                             .assign({.action = "inventory.inspect", .actor = "reader", .target = "warehouse", .detail = "quote \" newline\n UTF8 é"},
                                     stream,
                                     sequence)
                             .wasAdmitted()
                        || !(*sink)->accept(event))
                        return 2;
                }
            }
            (*sink)->close();
            return 0;
        }
        if (mode != "report")
            return 2;
        RetainedPosition position;
        const auto       report = LogVerifier(**medium, **authority, position).verify();
        const auto       image  = LogImage::read(**medium);
        AuditEvidence    evidence;
        evidence.source = args[2];
        std::cout << auditProjectionJson(evidence, report, image, "test-oracle");
        return std::cout ? 0 : 2;
    } catch (const std::exception& error) {
        std::println(std::cerr, "{}", error.what());
        return 2;
    }
}
