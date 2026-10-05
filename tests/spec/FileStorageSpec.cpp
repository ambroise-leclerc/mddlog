/** @brief Linux file backend conformity, real restart and deterministic system-call failures (#114). */
#include "framework/StorageMediumConformance.hpp"

#include <cerrno>
#include <cstdlib>

#include <sys/stat.h>
#include <unistd.h>

import std;
import mddlog.adapter.filestoragemedium;
import mddlog.adapter.auditstore;
import mddlog.core.auditevent;
import speclab;

namespace {
using namespace mddlog::adapter;

struct Directory {
    Directory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "mddlog-114-XXXXXX").string();
        const char* made    = ::mkdtemp(pattern.data());
        if (made == nullptr)
            throw std::runtime_error("cannot create test directory");
        path = made;
    }
    Directory(const Directory&)            = delete;
    Directory& operator=(const Directory&) = delete;
    Directory(Directory&&)                 = delete;
    Directory& operator=(Directory&&)      = delete;
    ~Directory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    [[nodiscard]] FileStorageConfig config() const {
        return {.directory = path, .maxSegments = 8, .maxSegmentBytes = 2048, .maxReadBytes = 2048};
    }
    std::filesystem::path path;
};

struct FaultCalls final : FileStorageCalls {
    std::ptrdiff_t write(int descriptor, std::span<const std::uint8_t> bytes) override {
        ++writeCount;
        if (interruptWrite) {
            interruptWrite = false;
            errno          = EINTR;
            return -1;
        }
        if (failWrite != 0 && writeCount >= failWrite) {
            errno = writeError;
            return -1;
        }
        if (zeroWrite)
            return 0;
        return FileStorageCalls::write(descriptor, bytes.first(std::min(bytes.size(), writeLimit)));
    }
    std::ptrdiff_t read(int descriptor, std::span<std::uint8_t> bytes, std::uint64_t offset) override {
        if (interruptRead) {
            interruptRead = false;
            errno         = EINTR;
            return -1;
        }
        if (failRead) {
            errno = EIO;
            return -1;
        }
        return FileStorageCalls::read(descriptor, bytes.first(std::min(bytes.size(), readLimit)), offset);
    }
    int sync(int descriptor) override {
        ++syncCount;
        struct stat metadata{};
        if (::fstat(descriptor, &metadata) == 0)
            syncDirectories.push_back(S_ISDIR(metadata.st_mode));
        if (interruptSync) {
            interruptSync = false;
            errno         = EINTR;
            return -1;
        }
        if (syncCount == failSync) {
            errno = EIO;
            return -1;
        }
        return FileStorageCalls::sync(descriptor);
    }
    std::size_t       writeCount     = 0;
    std::size_t       syncCount      = 0;
    std::size_t       writeLimit     = 2;
    std::size_t       readLimit      = 2;
    std::size_t       failWrite      = 0;
    std::size_t       failSync       = 0;
    int               writeError     = ENOSPC;
    bool              interruptWrite = false;
    bool              interruptRead  = false;
    bool              interruptSync  = false;
    bool              zeroWrite      = false;
    bool              failRead       = false;
    std::vector<bool> syncDirectories;
};

constexpr std::array<std::uint8_t, 4> bytes{1, 2, 3, 4};

const speclab::Register conformity{"File storage and in-memory storage share the StorageMedium invariants", "integration", [] {
                                       return speclab::Test("file-storage-conformance")
                                           .Then("the common contract passes on both backends",
                                                 [] {
                                                     speclab::core::Checks checks;
                                                     InMemoryStorageMedium memory(8, false);
                                                     mddlog::tests::checkStorageMediumConformance(memory, checks);
                                                     Directory directory;
                                                     auto      made = FileStorageMedium::create(directory.config());
                                                     checks.expect(made.has_value(), "file backend starts");
                                                     if (made)
                                                         mddlog::tests::checkStorageMediumConformance(**made, checks);
                                                     checks.raise();
                                                 })
                                           .Execute();
                                   }};

const speclab::Register restart{
    "Real file storage reopens without claiming deployment qualification",
    "integration",
    [] {
        return speclab::Test("file-storage-restart")
            .Then("a persisting sink writes records that a new read-only instance checks",
                  [] {
                      speclab::core::Checks checks;
                      Directory             directory;
                      {
                          auto          medium = FileStorageMedium::create(directory.config());
                          StorageConfig config;
                          config.segmentSize        = 2048;
                          config.segmentCount       = 8;
                          config.maxProducerStreams = 1;
                          auto sink                 = PersistingAuditSink::create(**medium, config);
                          for (std::uint64_t sequence = 1; sequence <= 3; ++sequence) {
                              mddlog::core::AuditEvent event;
                              (void)event.assign({.action = "therapy.rate.set", .target = "pump/channel-A"}, "pump:boot-1", sequence);
                              checks.expect((*sink)->accept(event), "sink hands event to medium");
                          }
                          (*sink)->flush();
                          checks.expect((*sink)->durablePosition("pump:boot-1") == 0, "unqualified filesystem does not advance durable position");
                      }
                      auto config       = directory.config();
                      config.access     = FileAccess::ReadOnly;
                      auto       reader = FileStorageMedium::create(config);
                      const auto stored = readStoredStream(**reader, "pump:boot-1");
                      checks.expect(stored.records().size() == 3 && stored.trailingBytesOfLastSegment() == 0,
                                    "real files preserve complete frames across object restart");
                      AuditChainVerifier verifier("pump:boot-1");
                      for (const auto& record : stored.records())
                          checks.expect(verifier.check(record.bytes, record.digest) == ChainFinding::Ok, "chain checks from its initial digest");
                      const auto segments = (*reader)->segments();
                      const auto ref      = segments->front().segment;
                      checks.expect((*reader)->open({.bytes = bytes}).status == OpenStatus::Failed, "read-only refuses create");
                      checks.expect((*reader)->append(ref, bytes).status == AppendStatus::Failed && !(*reader)->reclaim(ref),
                                    "read-only refuses append and reclaim");
                      checks.expect((*reader)->sync(ref, 0) == SyncAnswer::Failed, "reader cannot publish durable confirmation");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register barriers{
    "Only declared eligibility and completed data and directory barriers allow Durable",
    "integration",
    [] {
        return speclab::Test("file-storage-barriers")
            .Then("short transfers and EINTR are completed, then file and directory are synced",
                  [] {
                      speclab::core::Checks checks;
                      Directory             directory;
                      FaultCalls            calls;
                      calls.interruptWrite = true;
                      calls.interruptRead  = true;
                      calls.interruptSync  = true;
                      auto config          = directory.config();
                      config.durability    = FileDurability::QualifiedFsync;
                      auto       made      = FileStorageMedium::create(config, &calls);
                      const auto opened    = (*made)->open({.bytes = bytes});
                      checks.expect(opened.status == OpenStatus::Opened && calls.writeCount == 3, "interrupted and short opening completed");
                      checks.expect((*made)->read(opened.segment, 0, 4) == std::optional(std::vector<std::uint8_t>(bytes.begin(), bytes.end())),
                                    "short interrupted reads completed");
                      checks.expect((*made)->sync(opened.segment, 5) == SyncAnswer::Failed && calls.syncCount == 0,
                                    "offset beyond written end cannot be confirmed");
                      checks.expect((*made)->sync(999, 0) == SyncAnswer::Failed && (*made)->lastError()->nativeError == ENOENT,
                                    "missing segment keeps the original syscall error");
                      checks.expect((*made)->sync(opened.segment, 4) == SyncAnswer::Durable, "both barriers acknowledged under test declaration");
                      checks.expect(calls.syncDirectories == std::vector<bool>{false, false, true}, "EINTR retries file barrier before directory barrier");
                      checks.expect((*made)->reclaim(opened.segment) && calls.syncDirectories.back(), "removal acknowledges directory barrier");
                      checks.raise();
                  })
            .Then("either barrier failure prevents confirmation and all later mutation",
                  [] {
                      speclab::core::Checks checks;
                      for (std::size_t failingBarrier : {1, 2}) {
                          Directory  directory;
                          FaultCalls calls;
                          calls.failSync    = failingBarrier;
                          auto config       = directory.config();
                          config.durability = FileDurability::QualifiedFsync;
                          auto       made   = FileStorageMedium::create(config, &calls);
                          const auto opened = (*made)->open({.bytes = bytes});
                          checks.expect((*made)->sync(opened.segment, 4) == SyncAnswer::Failed && (*made)->mutationsStopped(), "barrier error stops writer");
                          checks.expect((*made)->sync(opened.segment, 4) == SyncAnswer::Failed
                                            && (*made)->append(opened.segment, bytes).status == AppendStatus::Failed,
                                        "retry cannot manufacture confirmation after writeback error");
                          checks.expect((*made)->lastError()->nativeError == EIO && (*made)->read(opened.segment, 0, 4).has_value(),
                                        "failure stays observable and bytes inspectable");
                      }
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register failures{
    "File storage preserves failures, partial evidence and explicit capacity",
    "integration",
    [] {
        return speclab::Test("file-storage-failures")
            .Then("ENOSPC, access failure and zero progress never return successful writes",
                  [] {
                      speclab::core::Checks checks;
                      for (int code : {ENOSPC, EACCES, EIO}) {
                          Directory  directory;
                          FaultCalls calls;
                          calls.failWrite   = 2;
                          calls.writeError  = code;
                          auto       made   = FileStorageMedium::create(directory.config(), &calls);
                          const auto opened = (*made)->open({.bytes = bytes});
                          checks.expect(opened.status == (code == ENOSPC ? OpenStatus::NoSpace : OpenStatus::Failed), "partial open returns failure");
                          const auto held = (*made)->segments();
                          checks.expect(held && held->size() == 1 && held->front().size == 2, "torn creation retained for recovery inspection");
                          checks.expect((*made)->mutationsStopped() && (*made)->lastError()->nativeError == code, "writer stopped with original errno");
                      }
                      Directory  directory;
                      FaultCalls calls;
                      auto       made   = FileStorageMedium::create(directory.config(), &calls);
                      const auto opened = (*made)->open({.bytes = bytes});
                      calls.failWrite   = calls.writeCount + 2;
                      checks.expect((*made)->append(opened.segment, bytes).status == AppendStatus::Failed, "partial append fails");
                      checks.expect((*made)->segments()->front().size == 6 && (*made)->mutationsStopped(),
                                    "partial suffix remains and cannot be appended over");
                      calls.failRead = true;
                      checks.expect(!(*made)->read(opened.segment, 0, 1) && (*made)->lastError()->issue == FileStorageIssue::Read,
                                    "read failure differs from empty range");
                      Directory  zeroDirectory;
                      FaultCalls zero;
                      zero.zeroWrite  = true;
                      auto zeroMedium = FileStorageMedium::create(zeroDirectory.config(), &zero);
                      checks.expect((*zeroMedium)->open({.bytes = bytes}).status == OpenStatus::Failed && zero.writeCount == 1, "zero progress terminates");
                      checks.raise();
                  })
            .Then("limits and failed removal are observable",
                  [] {
                      speclab::core::Checks checks;
                      Directory             directory;
                      FaultCalls            calls;
                      auto                  config = directory.config();
                      config.maxSegments           = 1;
                      config.maxSegmentBytes       = 4;
                      config.maxReadBytes          = 4;
                      auto       made              = FileStorageMedium::create(config, &calls);
                      const auto opened            = (*made)->open({.bytes = bytes});
                      checks.expect((*made)->open({.bytes = bytes}).status == OpenStatus::NoSpace, "segment capacity enforced");
                      checks.expect((*made)->append(opened.segment, bytes).status == AppendStatus::Failed, "byte capacity enforced before writing");
                      checks.expect(!(*made)->read(opened.segment, 0, 5), "read allocation bounded");
                      checks.expect((*made)->sync(opened.segment, 4) == SyncAnswer::Unsupported && calls.syncCount == 0, "default sync is always unsupported");
                      calls.failSync = 1;
                      checks.expect(!(*made)->reclaim(opened.segment) && (*made)->mutationsStopped(), "failed directory barrier does not acknowledge removal");
                      checks.expect((*made)->segments()->empty(), "unacknowledged unlink can already be visible");
                      checks.raise();
                  })
            .Execute();
    }};

const speclab::Register ownership{"File storage refuses ambiguous inventory and competing access", "integration", [] {
                                      return speclab::Test("file-storage-ownership")
                                          .Then("exclusive writers and shared offline readers release locks by lifetime",
                                                [] {
                                                    speclab::core::Checks checks;
                                                    Directory             directory;
                                                    {
                                                        auto writer = FileStorageMedium::create(directory.config());
                                                        auto rival  = FileStorageMedium::create(directory.config());
                                                        checks.expect(!rival && rival.error().issue == FileStorageIssue::Busy, "second writer refused");
                                                        auto config   = directory.config();
                                                        config.access = FileAccess::ReadOnly;
                                                        checks.expect(!FileStorageMedium::create(config), "reader refused while writer active");
                                                    }
                                                    auto config   = directory.config();
                                                    config.access = FileAccess::ReadOnly;
                                                    auto reader   = FileStorageMedium::create(config);
                                                    checks.expect(reader && FileStorageMedium::create(config).has_value(),
                                                                  "shared readers after writer destroyed");
                                                    checks.raise();
                                                })
                                          .Then("unexpected names, symlinks, hardlinks and public directory permissions are refused",
                                                [] {
                                                    speclab::core::Checks checks;
                                                    for (int kind : {0, 1, 2, 3}) {
                                                        Directory directory;
                                                        if (kind == 3) {
                                                            (void)::chmod(directory.path.c_str(), 0755);
                                                        } else {
                                                            auto medium = FileStorageMedium::create(directory.config());
                                                            (void)(*medium)->open({.bytes = bytes});
                                                            medium->reset();
                                                            const auto original = directory.path / "0000000000000001.mdl";
                                                            if (kind == 0)
                                                                std::filesystem::rename(original, directory.path / "invalid-name");
                                                            else if (kind == 1) {
                                                                std::filesystem::remove(original);
                                                                std::filesystem::create_symlink("missing", original);
                                                            } else
                                                                std::filesystem::create_hard_link(original, directory.path / "0000000000000002.mdl");
                                                        }
                                                        auto refused = FileStorageMedium::create(directory.config());
                                                        checks.expect(!refused, "ambiguous or unowned directory refused");
                                                    }
                                                    Directory directory;
                                                    auto      bad   = directory.config();
                                                    bad.maxSegments = 0;
                                                    checks.expect(!FileStorageMedium::create(bad), "invalid limits refused");
                                                    auto invalidPath       = directory.config();
                                                    invalidPath.directory  = std::filesystem::path(directory.path.native() + std::string("\0suffix", 7));
                                                    const auto refusedPath = FileStorageMedium::create(invalidPath);
                                                    checks.expect(!refusedPath && refusedPath.error().issue == FileStorageIssue::InvalidConfig,
                                                                  "embedded NUL cannot redirect a truncated system path");
                                                    auto good = FileStorageMedium::create(directory.config());
                                                    checks.expect(good.has_value(), "failed startup leaked no directory lock");
                                                    const auto opened = (*good)->open({.bytes = bytes});
                                                    const auto name   = directory.path / "0000000000000001.mdl";
                                                    (void)::chmod(name.c_str(), 0644);
                                                    checks.expect(!(*good)->reclaim(opened.segment) && (*good)->mutationsStopped()
                                                                      && std::filesystem::exists(name),
                                                                  "reclaim validates metadata even for a cached descriptor");
                                                    checks.raise();
                                                })
                                          .Execute();
                                  }};

}  // namespace

namespace {
const speclab::Register healthFailures{"File storage errors reach sink health without advancing durable claims", "integration", [] {
                                           return speclab::Test("file-storage-health")
                                               .Then("each barrier failure ends the stream without durable progress",
                                                     [] {
                                                         speclab::core::Checks checks;
                                                         for (std::size_t ordinal : {1, 2}) {
                                                             Directory  directory;
                                                             FaultCalls calls;
                                                             calls.failSync       = ordinal;
                                                             auto files           = directory.config();
                                                             files.durability     = FileDurability::QualifiedFsync;
                                                             auto          medium = FileStorageMedium::create(files, &calls);
                                                             StorageConfig config;
                                                             config.segmentSize            = 2048;
                                                             config.segmentCount           = 8;
                                                             config.maxProducerStreams     = 1;
                                                             auto                     sink = PersistingAuditSink::create(**medium, config);
                                                             mddlog::core::AuditEvent event;
                                                             (void)event.assign({.action = "therapy.rate.set", .target = "pump/channel-A"}, "health:boot-1", 1);
                                                             (void)(*sink)->accept(event);
                                                             const auto health = (*sink)->health();
                                                             checks.expect(health.counters.syncFailures == 1 && health.lastIssue == StorageIssue::SyncFailed,
                                                                           "sink publishes barrier failure separately from audit records");
                                                             checks.expect((*sink)->durablePosition("health:boot-1") == 0
                                                                               && !(*sink)->durableClaim("health:boot-1"),
                                                                           "no claim can cover a failed barrier");
                                                             checks.expect((*medium)->mutationsStopped(), "backend stays stopped");
                                                         }
                                                         checks.raise();
                                                     })
                                               .Execute();
                                       }};
}  // namespace
