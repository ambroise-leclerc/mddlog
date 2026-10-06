/** @brief Reader checkpoint restart, rollback, concurrent generations and failed barriers (#115). */
#include <cerrno>
#include <cstdlib>

#include <sys/stat.h>
import std;
import speclab;
import mddlog.adapter.fileretainedposition;
import mddlog.adapter.filestoragemedium;
import mddlog.core.auditevent;

namespace {
using namespace mddlog::adapter;
struct Directory {
    Directory() {
        std::string pattern = (std::filesystem::temp_directory_path() / "mddlog-115-XXXXXX").string();
        const char* made    = ::mkdtemp(pattern.data());
        if (made == nullptr)
            throw std::runtime_error("checkpoint directory");
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
    std::filesystem::path path;
};
[[nodiscard]] RetainedPosition position(std::uint64_t head = 2) {
    RetainedPosition out;
    out.raiseHead("witness", head);
    out.raiseAnchor("device/boot", {.position = head, .digest = chainInitialValue, .counter = head});
    return out;
}
struct FaultCalls final : FileStorageCalls {
    std::ptrdiff_t write(int descriptor, std::span<const std::uint8_t> bytes) override {
        if (interrupt) {
            interrupt = false;
            errno     = EINTR;
            return -1;
        }
        ++writeCount;
        if (failWrite && writeCount >= 2) {
            errno = ENOSPC;
            return -1;
        }
        return FileStorageCalls::write(descriptor, bytes.first(std::min(bytes.size(), std::size_t{7})));
    }
    int sync(int descriptor) override {
        ++syncCount;
        if (syncCount == failSync) {
            errno = EIO;
            return -1;
        }
        return FileStorageCalls::sync(descriptor);
    }
    int replaceMetadata(int root, const char* temporary, const char* destination) override {
        if (failRename) {
            errno = EIO;
            return -1;
        }
        return FileStorageCalls::replaceMetadata(root, temporary, destination);
    }
    bool interrupt  = true;
    bool failWrite  = false;
    bool failRename = false;
    int  failSync   = 0;
    int  syncCount  = 0;
    int  writeCount = 0;
};
void expectIssue(const auto& result, RetainedFileIssue issue, std::string_view message) {
    speclab::core::Checks checks;
    checks.expect(!result && result.error().issue == issue, message);
    checks.raise();
}
// NOLINTNEXTLINE(cppcoreguidelines-interfaces-global-init): registry stores a lambda; stream modes are used only when the scenario executes.
const speclab::Register registered{
    "Persistent reader checkpoints reject rollback and failed restoration",
    "unit",
    [] {
        return speclab::Test("audit-retained-file")
            .Then("missing state is explicit and initialization is never automatic",
                  [] {
                      Directory            directory;
                      FileRetainedPosition store{directory.path, "witness"};
                      expectIssue(store.load(), RetainedFileIssue::Missing, "missing restoration");
                      expectIssue(store.save(position(), 0), RetainedFileIssue::Missing, "save cannot enroll");
                      speclab::core::Checks checks;
                      const auto            enrolled = store.initialize(position());
                      checks.expect(enrolled && *enrolled == 1, "explicit initialization");
                      checks.raise();
                      expectIssue(store.initialize(position()), RetainedFileIssue::Conflict, "cannot overwrite initialization");
                      std::filesystem::remove(directory.path / "retained.bin");
                      expectIssue(store.save(position(), 1), RetainedFileIssue::Missing, "loss does not become first use");
                  })
            .Then("restart restores heads, digests, counters and irreversible retirement",
                  [] {
                      Directory        directory;
                      RetainedPosition held = position();
                      held.raiseAnchor("device/boot", {.position = 2, .digest = chainInitialValue, .counter = 2, .retired = true});
                      {
                          FileRetainedPosition store{directory.path, "witness"};
                          if (!store.initialize(held))
                              throw std::runtime_error("initialize");
                      }
                      FileRetainedPosition  restarted{directory.path, "witness"};
                      const auto            restored = restarted.load();
                      speclab::core::Checks checks;
                      checks.expect(restored && restored->generation == 1 && restored->position.allHeads() == held.allHeads()
                                        && restored->position.allAnchors() == held.allAnchors(),
                                    "exact checkpoint after reopening");
                      checks.raise();
                      expectIssue(restarted.save(position(), 1), RetainedFileIssue::Regression, "retirement cannot be undone");
                      FileRetainedPosition migrated{directory.path, "other-provider"};
                      expectIssue(migrated.load(), RetainedFileIssue::ProviderMismatch, "no implicit provider migration");
                  })
            .Then("concurrent readers cannot overwrite a newer generation or lower its checkpoints",
                  [] {
                      Directory            directory;
                      FileRetainedPosition first{directory.path, "witness"};
                      FileRetainedPosition second{directory.path, "witness"};
                      if (!first.initialize(position()))
                          throw std::runtime_error("initialize");
                      const auto old = second.load();
                      if (!old || !first.save(position(3), 1))
                          throw std::runtime_error("save");
                      expectIssue(second.save(position(4), old->generation), RetainedFileIssue::Conflict, "stale generation");
                      expectIssue(second.save(position(), 2), RetainedFileIssue::Regression, "lower head and anchor");
                      RetainedPosition conflicting;
                      conflicting.raiseHead("witness", 4);
                      auto digest    = chainInitialValue;
                      digest.front() = 1;
                      conflicting.raiseAnchor("device/boot", {.position = 3, .digest = digest, .counter = 4});
                      expectIssue(second.save(conflicting, 2), RetainedFileIssue::Regression, "same position conflicting digest");
                      RetainedPosition missing;
                      missing.raiseHead("witness", 4);
                      expectIssue(second.save(missing, 2), RetainedFileIssue::Regression, "cannot discard a stream");
                  })
            .Then("simultaneous readers serialize and only one generation wins",
                  [] {
                      Directory            directory;
                      FileRetainedPosition first{directory.path, "witness"};
                      FileRetainedPosition second{directory.path, "witness"};
                      if (!first.initialize(position()))
                          throw std::runtime_error("initialize");
                      std::barrier                                    start{3};
                      std::expected<std::uint64_t, RetainedFileError> left;
                      std::expected<std::uint64_t, RetainedFileError> right;
                      std::jthread                                    one([&] {
                          start.arrive_and_wait();
                          left = first.save(position(3), 1);
                      });
                      std::jthread                                    two([&] {
                          start.arrive_and_wait();
                          right = second.save(position(4), 1);
                      });
                      start.arrive_and_wait();
                      one.join();
                      two.join();
                      const auto            restored = first.load();
                      speclab::core::Checks checks;
                      checks.expect(left.has_value() != right.has_value(), "one update wins");
                      const auto& lost = left ? right : left;
                      checks.expect(!lost && (lost.error().issue == RetainedFileIssue::Busy || lost.error().issue == RetainedFileIssue::Conflict),
                                    "loser must reload");
                      checks.expect(restored && restored->generation == 2 && restored->position.head("witness") == (left ? 3 : 4), "complete winner snapshot");
                      checks.raise();
                  })
            .Then("every truncated prefix and oversized file is rejected without restoration",
                  [] {
                      Directory            directory;
                      FileRetainedPosition store{directory.path, "witness"};
                      if (!store.initialize(position()))
                          throw std::runtime_error("initialize");
                      const auto                      path = directory.path / "retained.bin";
                      std::ifstream                   input(path, std::ios::binary);
                      const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{input}, {});
                      input.close();
                      for (std::size_t count = 0; count < bytes.size(); ++count) {
                          {
                              std::ofstream output(path, std::ios::binary | std::ios::trunc);
                              for (auto byte : std::span{bytes}.first(count))
                                  output.put(static_cast<char>(byte));
                          }
                          expectIssue(store.load(), RetainedFileIssue::Invalid, "truncated snapshot");
                      }
                      std::filesystem::resize_file(path, (1024 * 1024) + 1);
                      expectIssue(store.load(), RetainedFileIssue::Invalid, "bounded restoration");
                      std::filesystem::remove(path);
                      std::filesystem::create_symlink(directory.path / "elsewhere", path);
                      expectIssue(store.load(), RetainedFileIssue::Read, "no symlink traversal");
                  })
            .Then("corruption, unknown version and unsafe access fail closed",
                  [] {
                      Directory            directory;
                      FileRetainedPosition store{directory.path, "witness"};
                      if (!store.initialize(position()))
                          throw std::runtime_error("initialize");
                      const auto                path = directory.path / "retained.bin";
                      std::ifstream             input(path, std::ios::binary);
                      std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>{input}, {});
                      bytes.at(8)         = 2;
                      const auto checksum = sha256(std::span{bytes}.first(bytes.size() - sha256DigestSize));
                      std::ranges::copy(checksum, bytes.end() - static_cast<std::ptrdiff_t>(sha256DigestSize));
                      {
                          std::ofstream output(path, std::ios::binary | std::ios::trunc);
                          for (auto byte : bytes)
                              output.put(static_cast<char>(byte));
                      }
                      expectIssue(store.load(), RetainedFileIssue::UnsupportedVersion, "unknown version");
                      bytes.back() = static_cast<std::uint8_t>(bytes.back() + 1);
                      {
                          std::ofstream output(path, std::ios::binary | std::ios::trunc);
                          for (auto byte : bytes)
                              output.put(static_cast<char>(byte));
                      }
                      expectIssue(store.load(), RetainedFileIssue::Invalid, "checksum corruption");
                      expectIssue(store.save(position(), 1), RetainedFileIssue::Invalid, "corruption cannot be overwritten");
                      if (::chmod(directory.path.c_str(), S_IRWXU | S_IRWXG) != 0)
                          throw std::runtime_error("chmod");
                      expectIssue(store.load(), RetainedFileIssue::Access, "shared reader directory refused");
                  })
            .Then(
                "partial writes and failed barriers preserve the old state or require reconciliation",
                [] {
                    for (int stage = 0; stage < 4; ++stage) {
                        Directory            directory;
                        FileRetainedPosition initial{directory.path, "witness"};
                        if (!initial.initialize(position()))
                            throw std::runtime_error("initialize");
                        FaultCalls calls;
                        calls.failWrite = stage == 0;
                        if (stage == 1)
                            calls.failSync = 1;
                        if (stage == 3)
                            calls.failSync = 2;
                        calls.failRename = stage == 2;
                        FileRetainedPosition store{directory.path, "witness", &calls};
                        const auto           saved = store.save(position(3), 1);
                        expectIssue(saved, stage == 1 || stage == 3 ? RetainedFileIssue::Sync : RetainedFileIssue::Write, "failed save is not durable success");
                        const auto            restored = initial.load();
                        speclab::core::Checks checks;
                        checks.expect(restored && restored->generation == (stage == 3 ? std::uint64_t{2} : std::uint64_t{1}),
                                      "old state before rename; new visible state after failed directory sync");
                        checks.raise();
                    }
                    Directory             directory;
                    FaultCalls            calls;
                    FileRetainedPosition  store{directory.path, "witness", &calls};
                    speclab::core::Checks checks;
                    checks.expect(store.initialize(position()).has_value() && store.load().has_value(), "EINTR and short writes retry");
                    checks.raise();
                })
            .Then("an old log and old provider remain rolled back after reader restart",
                  [] {
                      Directory                directory;
                      InMemoryAnchorProvider   provider{"witness"};
                      AuditChain               chain{"device/boot"};
                      mddlog::core::AuditEvent event;
                      (void)event.assign({.action = "change", .target = "pump"}, "device/boot", 1);
                      const auto first = chain.append(event);
                      if (!first)
                          throw std::runtime_error("chain");
                      (void)provider.advance(makeAnchorClaim("device/boot", 1, first->digest));
                      const auto oldProvider = provider.snapshot();
                      (void)event.assign({.action = "change", .target = "pump"}, "device/boot", 2);
                      const auto second = chain.append(event);
                      if (!second)
                          throw std::runtime_error("chain");
                      (void)provider.advance(makeAnchorClaim("device/boot", 2, second->digest));
                      RetainedPosition                  held;
                      AnchorVerifier                    verifier{provider, held};
                      const std::array<StoredRecord, 2> records{
                          {{.bytes = first->canonical.bytes(), .digest = first->digest}, {.bytes = second->canonical.bytes(), .digest = second->digest}}
                      };
                      const auto verified = verifier.verify("device/boot", records);
                      if (verified.verdict != Verdict::Anchored)
                          throw std::runtime_error("verification");
                      {
                          FileRetainedPosition store{directory.path, "witness"};
                          if (!store.initialize(held))
                              throw std::runtime_error("initialize");
                      }
                      provider.restore(oldProvider);
                      FileRetainedPosition restarted{directory.path, "witness"};
                      auto                 restored = restarted.load();
                      if (!restored)
                          throw std::runtime_error("restore");
                      AnchorVerifier        afterRestart{provider, restored->position};
                      const auto            report = afterRestart.verify("device/boot", std::span{records}.first(1));
                      speclab::core::Checks checks;
                      checks.expect(report.verdict == Verdict::RolledBack, "restored reader detects combined rollback");
                      checks.raise();
                  })
            .Execute();
    }};
}  // namespace
