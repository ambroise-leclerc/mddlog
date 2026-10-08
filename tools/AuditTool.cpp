/** @brief Optional Linux audit inspection, export and replay command (#119). */
#include <cerrno>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
import std;
import mddlog.core.auditevent;
import mddlog.adapter.auditprojection;
import mddlog.adapter.filestoragemedium;
import mddlog.adapter.fileretainedposition;
import mddlog.adapter.unixanchorprovider;

namespace {
using namespace mddlog::adapter;
constexpr std::size_t configBytes = 65536;
using Options                     = std::map<std::string, std::string>;
const std::set<std::string> optionNames{"config",
                                        "source",
                                        "archive",
                                        "output",
                                        "format",
                                        "socket",
                                        "provider-id",
                                        "server-uid",
                                        "timeout-ms",
                                        "retained",
                                        "update-retained",
                                        "accept-embedded-provider",
                                        "stream",
                                        "max-anchor-age-ns",
                                        "verification-time-ns",
                                        "max-archive-bytes",
                                        "max-segments",
                                        "max-segment-bytes",
                                        "max-total-bytes",
                                        "read-chunk-bytes",
                                        "max-read-bytes",
                                        "max-streams",
                                        "max-records",
                                        "max-provider-entries",
                                        "max-provider-text-bytes",
                                        "max-provider-calls",
                                        "max-integrity-faults"};
const std::set<std::string> flags{"update-retained", "accept-embedded-provider"};

[[nodiscard]] std::string get(const Options& options, std::string_view key, const std::string& fallback = {}) {
    const auto found = options.find(std::string(key));
    return found == options.end() ? fallback : found->second;
}
[[nodiscard]] std::uint64_t number(std::string_view text) {
    std::uint64_t value     = 0;
    const auto [end, error] = std::from_chars(text.data(), std::to_address(text.end()), value);
    if (text.empty() || error != std::errc{} || end != std::to_address(text.end()))
        throw std::invalid_argument("expected unsigned decimal integer");
    return value;
}
[[nodiscard]] std::int64_t signedNumber(std::string_view text) {
    std::int64_t value      = 0;
    const auto [end, error] = std::from_chars(text.data(), std::to_address(text.end()), value);
    if (text.empty() || error != std::errc{} || end != std::to_address(text.end()))
        throw std::invalid_argument("expected signed nanoseconds");
    return value;
}
[[nodiscard]] bool flag(const Options& options, std::string_view key) {
    const auto text = get(options, key, "false");
    if (text != "true" && text != "false")
        throw std::invalid_argument("boolean option must be true or false");
    return text == "true";
}
[[nodiscard]] std::vector<std::uint8_t> readFile(const std::filesystem::path& path, std::size_t maximum) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fixed POSIX open flags, no creation.
    detail::FileDescriptor file(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    struct stat            info{};
    if (file.get() < 0 || ::fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
        throw std::runtime_error("cannot read regular input file: check path and permissions");
    if (std::cmp_greater(info.st_size, maximum))
        throw std::length_error("input file exceeds configured byte budget");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(info.st_size));
    std::size_t               offset = 0;
    while (offset < bytes.size()) {
        const auto got = ::read(file.get(), std::span(bytes).subspan(offset).data(), bytes.size() - offset);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            throw std::runtime_error("input truncated or unreadable");
        offset += static_cast<std::size_t>(got);
    }
    std::uint8_t extra = 0;
    if (::read(file.get(), &extra, 1) != 0)
        throw std::runtime_error("input changed during read");
    return bytes;
}
void writeFile(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path(".");
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fixed POSIX directory flags.
    detail::FileDescriptor directory(::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (directory.get() < 0)
        throw std::runtime_error("cannot open output directory");
    const auto name = path.filename().string();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg): fixed exclusive flags and private mode.
    detail::FileDescriptor file(::openat(directory.get(), name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR));
    if (file.get() < 0)
        throw std::runtime_error("cannot create output: use a new filename in a writable directory");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto wrote = ::write(file.get(), std::span(bytes).subspan(offset).data(), bytes.size() - offset);
            if (wrote < 0 && errno == EINTR)
                continue;
            if (wrote <= 0)
                throw std::runtime_error("output write failed");
            offset += static_cast<std::size_t>(wrote);
        }
        if (::fsync(file.get()) != 0 || ::fsync(directory.get()) != 0)
            throw std::runtime_error("output sync failed");
    } catch (...) {
        (void)::unlinkat(directory.get(), name.c_str(), 0);
        throw;
    }
}
void outputText(const Options& options, std::string_view text, std::size_t maximum) {
    if (text.size() > maximum)
        throw std::length_error("report exceeds output budget");
    const auto path = get(options, "output");
    if (path.empty()) {
        std::cout << text;
        std::cout.flush();
        if (!std::cout)
            throw std::runtime_error("stdout write failed");
    } else {
        std::vector<std::uint8_t> bytes(text.begin(), text.end());
        writeFile(path, bytes);
    }
}
[[nodiscard]] Options parseOptions(std::span<char*> arguments) {
    Options commandLine;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string_view option(arguments[i]);
        if (!option.starts_with("--"))
            throw std::invalid_argument("options must be named; see --help");
        const std::string name(option.substr(2));
        if (!optionNames.contains(name) || commandLine.contains(name))
            throw std::invalid_argument("unknown or duplicate option: " + name);
        if (flags.contains(name))
            commandLine.emplace(name, "true");
        else {
            ++i;
            if (i == arguments.size() || std::string_view(arguments[i]).starts_with("--"))
                throw std::invalid_argument("missing value for " + name);
            commandLine.emplace(name, arguments[i]);
        }
    }
    Options out;
    if (const auto path = get(commandLine, "config"); !path.empty()) {
        const auto         bytes = readFile(path, configBytes);
        std::istringstream input(std::string(bytes.begin(), bytes.end()));
        std::string        line;
        bool               versionSeen = false;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty() || line.starts_with('#'))
                continue;
            const auto delimiter = line.find('=');
            if (delimiter == std::string::npos)
                throw std::invalid_argument("configuration requires key=value lines");
            const auto name  = line.substr(0, delimiter);
            const auto value = line.substr(delimiter + 1);
            if (name == "version") {
                if (versionSeen || value != "1")
                    throw std::invalid_argument("unsupported or duplicate configuration version");
                versionSeen = true;
            } else {
                if (!optionNames.contains(name) || name == "config" || value.empty() || !out.emplace(name, value).second)
                    throw std::invalid_argument("unknown, empty or duplicate configuration field: " + name);
            }
        }
        if (!versionSeen)
            throw std::invalid_argument("configuration requires version=1");
    }
    for (const auto& [key, value] : commandLine)
        out.insert_or_assign(key, value);
    for (const auto& [key, value] : out) {
        if (value.contains('\0'))
            throw std::invalid_argument("NUL in option: " + key);
        if (flags.contains(key))
            (void)flag(out, key);
    }
    return out;
}
[[nodiscard]] VerifierConfig verifierConfig(const Options& options) {
    VerifierConfig out;
    auto&          limits = out.resources;
    const auto     size   = [&](std::string_view key, std::uint64_t fallback) {
        const auto value = number(get(options, key, std::to_string(fallback)));
        if (value == 0 || value > std::numeric_limits<std::size_t>::max())
            throw std::invalid_argument("invalid budget: " + std::string(key));
        return static_cast<std::size_t>(value);
    };
    limits.maxSegments          = size("max-segments", limits.maxSegments);
    limits.maxSegmentBytes      = size("max-segment-bytes", limits.maxSegmentBytes);
    limits.maxTotalBytes        = size("max-total-bytes", limits.maxTotalBytes);
    limits.readChunkBytes       = size("read-chunk-bytes", limits.readChunkBytes);
    limits.maxReadBytes         = size("max-read-bytes", limits.maxReadBytes);
    limits.maxStreams           = size("max-streams", limits.maxStreams);
    limits.maxRecords           = size("max-records", limits.maxRecords);
    limits.maxProviderEntries   = size("max-provider-entries", limits.maxProviderEntries);
    limits.maxProviderTextBytes = size("max-provider-text-bytes", limits.maxProviderTextBytes);
    limits.maxProviderCalls     = size("max-provider-calls", limits.maxProviderCalls);
    limits.maxIntegrityFaults   = size("max-integrity-faults", limits.maxIntegrityFaults);
    if (options.contains("max-anchor-age-ns")) {
        const auto value = number(get(options, "max-anchor-age-ns"));
        if (value >= std::numeric_limits<std::int64_t>::max())
            throw std::invalid_argument("anchor age out of range");
        out.maxAnchorAge = std::chrono::nanoseconds{value};
    }
    if (options.contains("verification-time-ns"))
        out.verificationTime = mddlog::core::RawTime::available(
            std::chrono::sys_time<std::chrono::nanoseconds>{std::chrono::nanoseconds{signedNumber(get(options, "verification-time-ns"))}});
    return out;
}
[[nodiscard]] std::string humanReport(const LogReport& report, const LogImage& image, std::string_view trust, std::string_view retainedTrust) {
    std::string out = std::format(
        "Retained position: {}; trust: {}; exit {}; streams {}; unlisted {}; resource issue {}; unknown layout {}; headerless segments {}\n",
        retainedTrust,
        trust,
        std::to_underlying(auditToolExit(report, image)),
        report.streams.size(),
        report.unlisted.size(),
        std::to_underlying(report.resourceIssue),
        projection::optionalNumber(image.unknownLayoutVersion()),
        image.segmentsWithoutHeader());
    const auto stream = [&](const StreamReport& value) {
        out += std::format("{}: {}; cause {}; present {}..{}; anchored through {}; unanchored from {}; retained {}; age {}; rollback not excluded {}\n",
                           projection::quote(value.streamId),
                           verdictName(value.verdict),
                           std::to_underlying(value.cause),
                           value.firstRetained,
                           value.lastPresent,
                           projection::optionalNumber(value.anchoredThrough),
                           projection::optionalNumber(value.unanchoredFrom),
                           std::to_underlying(value.retained),
                           std::to_underlying(value.age),
                           value.rollbackNotExcluded());
    };
    for (const auto& value : report.streams) {
        stream(value.report);
        if (value.trailing)
            out += std::format("  residual: segment {}, offset {}, length {}\n", value.trailing->segmentIndex, value.trailing->offset, value.trailing->length);
    }
    for (const auto& value : report.unlisted)
        stream(value);
    for (const auto& note : report.notes)
        out += std::format("{}: {} at {}..{}\n", projection::quote(note.stream), boundaryName(note.kind), note.position, note.secondPosition);
    return out;
}
int run(std::span<char*> arguments) {
    if (arguments.empty() || std::string_view(arguments.front()) == "--help") {
        std::println("mddlog-audit inspect|export|verify|checkpoint-init [--config FILE] [named options]\n"
                     "inspect --source DIR [--format human|json]\nexport --source DIR --output NEW_FILE [--format evidence|json]\n"
                     "verify --archive FILE [--accept-embedded-provider | --socket PATH --provider-id ID --server-uid UID]\n"
                     "checkpoint-init --retained DIR --provider-id ID\n"
                     "Reader: --retained DIR [--update-retained]; selection: --stream ID (JSON only).\n"
                     "Budgets/time/configuration and exit codes 0,2,3,4: docs/audit-tools.md");
        return 0;
    }
    const std::string command(arguments.front());
    if (command != "inspect" && command != "export" && command != "verify" && command != "checkpoint-init")
        throw std::invalid_argument("unknown command; see --help");
    const auto options       = parseOptions(arguments.subspan(1));
    const auto declared      = verifierConfig(options);
    const auto archiveBudget = number(get(options, "max-archive-bytes", std::to_string(defaultEvidenceBytes)));
    if (archiveBudget == 0 || archiveBudget > std::numeric_limits<std::size_t>::max())
        throw std::invalid_argument("invalid archive budget");
    const auto maxBytes     = static_cast<std::size_t>(archiveBudget);
    const auto providerId   = get(options, "provider-id");
    const auto retainedPath = get(options, "retained");
    const auto update       = flag(options, "update-retained");
    if (options.contains("stream") && !mddlog::core::AuditEvent::validStreamId(get(options, "stream")))
        throw std::invalid_argument("invalid stream filter identity");
    if (!options.contains("socket") && (options.contains("server-uid") || options.contains("timeout-ms")))
        throw std::invalid_argument("server-uid and timeout-ms require --socket");
    if (command == "checkpoint-init") {
        for (const auto& [key, value] : options) {
            (void)value;
            if (key != "config" && key != "retained" && key != "provider-id")
                throw std::invalid_argument("option does not apply to checkpoint-init: " + key);
        }
        if (retainedPath.empty() || providerId.empty() || update || options.contains("source") || options.contains("archive"))
            throw std::invalid_argument("checkpoint-init requires only a reader directory and provider identity");
        FileRetainedPosition store(retainedPath, providerId);
        RetainedPosition     position;
        position.raiseHead(providerId, 0);
        const auto initialized = store.initialize(position);
        if (!initialized)
            throw std::runtime_error(std::format("checkpoint enrollment failed: issue {}; never overwrite or reset after a load failure",
                                                 std::to_underlying(initialized.error().issue)));
        std::println("Reader checkpoint enrolled; generation {}", *initialized);
        return 0;
    }
    if (update && (retainedPath.empty() || providerId.empty()))
        throw std::invalid_argument("--update-retained requires --retained and --provider-id");
    if (command != "verify" && options.contains("archive"))
        throw std::invalid_argument("--archive belongs to verify");
    if (command == "verify" && options.contains("source"))
        throw std::invalid_argument("verify uses --archive, not --source");
    if (command != "verify" && flag(options, "accept-embedded-provider"))
        throw std::invalid_argument("embedded trust applies to package replay only");
    const auto format = get(options, "format", command == "export" ? "evidence" : "human");
    if ((command == "export" && format != "json" && format != "evidence") || (command != "export" && format != "human" && format != "json"))
        throw std::invalid_argument("unsupported format for command");
    if (command == "export" && get(options, "output").empty())
        throw std::invalid_argument("export requires --output NEW_FILE");
    if (options.contains("stream") && format != "json")
        throw std::invalid_argument("event filtering is supported only for JSON projections; evidence is always whole");
    if (command != "verify" && !get(options, "output").empty()) {
        const std::filesystem::path destination(get(options, "output"));
        const auto                  parent = destination.has_parent_path() ? destination.parent_path() : std::filesystem::path(".");
        std::error_code             pathError;
        if (std::filesystem::equivalent(parent, get(options, "source"), pathError))
            throw std::invalid_argument("output must be outside the journal directory to preserve its inventory");
    }
    AuditEvidence                      evidence;
    std::unique_ptr<FileStorageMedium> fileMedium;
    if (command == "verify") {
        if (get(options, "archive").empty())
            throw std::invalid_argument("verify requires --archive FILE");
        evidence = decodeAuditEvidence(readFile(get(options, "archive"), maxBytes), declared.resources, maxBytes);
        if (options.contains("max-anchor-age-ns"))
            evidence.verification.maxAnchorAge = declared.maxAnchorAge;
        if (options.contains("verification-time-ns"))
            evidence.verification.verificationTime = declared.verificationTime;
    } else {
        if (get(options, "source").empty())
            throw std::invalid_argument("--source DIR is required");
        auto made = FileStorageMedium::create({.directory       = get(options, "source"),
                                               .maxSegments     = declared.resources.maxSegments,
                                               .maxSegmentBytes = declared.resources.maxSegmentBytes,
                                               .maxReadBytes    = declared.resources.readChunkBytes,
                                               .access          = FileAccess::ReadOnly});
        if (!made)
            throw std::runtime_error(std::format("cannot open journal: issue {}, errno {}; check private directory, inventory, bounds and writer lock",
                                                 std::to_underlying(made.error().issue),
                                                 made.error().nativeError));
        fileMedium = std::move(*made);
        evidence   = captureAuditEvidence(*fileMedium, get(options, "source"), declared);
    }
    std::string trust = "unavailable";
    if (!get(options, "socket").empty()) {
        if (flag(options, "accept-embedded-provider") || providerId.empty() || !options.contains("server-uid"))
            throw std::invalid_argument("socket trust requires provider-id and server-uid, with no embedded assumption");
        const auto uid     = number(get(options, "server-uid"));
        const auto timeout = number(get(options, "timeout-ms", "1000"));
        if (uid > std::numeric_limits<uid_t>::max() || timeout == 0 || timeout > std::numeric_limits<int>::max())
            throw std::invalid_argument("UID or timeout out of range");
        UnixAnchorProvider     provider({.socketPath = get(options, "socket"),
                                         .providerId = providerId,
                                         .serverUid  = static_cast<uid_t>(uid),
                                         .timeout    = std::chrono::milliseconds{timeout}});
        ResourceAnchorProvider bounded(provider, evidence.verification.resources);
        const auto             listing = bounded.streams();
        if (bounded.resourceIssue() != AuditResourceIssue::None)
            throw std::length_error("witness snapshot exceeds provider budget");
        evidence.provider.reset();
        evidence.providerProvenance = "unix:" + get(options, "socket") + ";uid=" + std::to_string(uid) + ";provider=" + providerId;
        if (const auto* snapshot = std::get_if<ProviderListing>(&listing)) {
            evidence.provider = *snapshot;
            trust             = "authenticated-unix-provider";
        } else if (const auto error = provider.lastError()) {
            std::println(std::cerr,
                         "Witness unavailable: issue {}, errno {}; check socket, server UID, reader ACL and deadline",
                         std::to_underlying(error->issue),
                         error->nativeError);
        }
    } else if (command == "verify" && flag(options, "accept-embedded-provider")) {
        trust = "accepted-embedded-assumption";
    } else {
        evidence.provider.reset();
    }
    std::string                           retainedTrust = command == "verify" ? "embedded-untrusted-snapshot" : "none";
    std::unique_ptr<FileRetainedPosition> reader;
    std::uint64_t                         generation = 0;
    if (!retainedPath.empty()) {
        if (providerId.empty())
            throw std::invalid_argument("--retained requires --provider-id");
        reader            = std::make_unique<FileRetainedPosition>(retainedPath, providerId);
        const auto loaded = reader->load();
        if (!loaded)
            throw std::runtime_error(std::format("checkpoint load failed: issue {}; restore the reader-owned store, do not enroll/reset silently",
                                                 std::to_underlying(loaded.error().issue)));
        retainedTrust     = "external-reader-store";
        evidence.retained = loaded->position;
        generation        = loaded->generation;
    }
    if (evidence.provider && !providerId.empty() && evidence.provider->providerId != providerId)
        throw std::invalid_argument("provider identity differs from configured reader identity");
    EvidenceMedium   medium(evidence);
    EvidenceProvider provider(evidence.provider);
    auto             candidate = evidence.retained;
    LogVerifier      verifier(medium, provider, candidate, evidence.verification);
    const auto       report = verifier.verify();
    const auto       image  = LogImage::read(medium, evidence.verification.resources);
    const auto       result = auditToolExit(report, image);
    if (update && (result == AuditToolExit::Impossible || result == AuditToolExit::Adverse || !evidence.provider || trust != "authenticated-unix-provider"))
        throw std::runtime_error("checkpoint update refused: requires an available authenticated witness and no adverse findings");
    if (command == "export" && format == "evidence") {
        if (result == AuditToolExit::Impossible)
            throw std::runtime_error("verification impossible; package not exported; inspect the journal and resource limits");
        writeFile(get(options, "output"), encodeAuditEvidence(evidence, maxBytes));
        std::cerr << humanReport(report, image, trust, retainedTrust);
    } else if (format == "json")
        outputText(options, auditProjectionJson(evidence, report, image, trust, get(options, "stream"), maxBytes, retainedTrust), maxBytes);
    else
        outputText(options, humanReport(report, image, trust, retainedTrust), maxBytes);
    if (update) {
        const auto saved = reader->save(candidate, generation);
        if (!saved)
            throw std::runtime_error(
                std::format("checkpoint save failed: issue {}; reload and reconcile before retrying", std::to_underlying(saved.error().issue)));
        std::println(std::cerr, "Reader checkpoint saved; generation {}", *saved);
    }
    return std::to_underlying(result);
}
}  // namespace

// NOLINTNEXTLINE(bugprone-exception-escape): all operational exceptions are caught; an allocation failure while reporting a failure may terminate.
int main(int argc, char** argv) {
    try {
        return run(std::span(argv, static_cast<std::size_t>(argc)).subspan(1));
    } catch (const std::exception& error) {
        std::println(std::cerr, "mddlog-audit: {}", error.what());
        return std::to_underlying(AuditToolExit::Impossible);
    }
}
