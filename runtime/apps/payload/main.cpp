#include "suffice/payload.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
namespace payload = suffice::payload;
using Clock = std::chrono::steady_clock;

namespace {
constexpr std::uint64_t fnv_basis = 14695981039346656037ULL;
constexpr std::uint64_t fnv_prime = 1099511628211ULL;

std::string path_text(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string quote(std::string_view text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result{"\""};
    for (const unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 32) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else result += static_cast<char>(c);
    }
    result += '"';
    return result;
}

template<class T>
T unsigned_integer(std::string_view value, std::string_view name) {
    T result{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || value.front() < '0' || value.front() > '9'
        || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::invalid_argument("invalid unsigned integer for --" + std::string(name) + ": " + std::string(value));
    return result;
}

struct Options {
    fs::path raw, packed, payload_file, program;
    std::optional<fs::path> output_directory, json_file;
    std::uint32_t channels{};
    payload::Backend backend{payload::Backend::Auto};
    payload::Validation validation{payload::Validation::Full};
    std::uint64_t first{};
    std::optional<std::uint64_t> count;
    std::uint64_t max_output_bytes{UINT64_MAX};
};

Options parse_options(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) != "run")
        throw std::invalid_argument("expected command 'run'; use --help for usage");
    const std::set<std::string_view> allowed{
        "raw", "packed", "payload", "channels", "program", "backend",
        "first-row", "row-count", "payload-validation", "write-rows", "json", "max-output-bytes"};
    std::map<std::string, std::string, std::less<>> values;
    for (int i = 2; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (!argument.starts_with("--"))
            throw std::invalid_argument("unexpected positional argument: " + std::string(argument));
        const auto name = argument.substr(2);
        if (!allowed.contains(name)) throw std::invalid_argument("unknown option: " + std::string(argument));
        if (values.contains(name)) throw std::invalid_argument("duplicate option: " + std::string(argument));
        if (++i == argc || std::string_view(argv[i]).starts_with("--"))
            throw std::invalid_argument("missing value for " + std::string(argument));
        if (std::string_view(argv[i]).empty())
            throw std::invalid_argument("empty value for " + std::string(argument));
        values.emplace(std::string(name), argv[i]);
    }
    const auto required = [&](std::string_view key) -> const std::string& {
        const auto found = values.find(key);
        if (found == values.end()) throw std::invalid_argument("required option: --" + std::string(key));
        return found->second;
    };
    Options result;
    result.raw = required("raw");
    result.packed = required("packed");
    result.payload_file = required("payload");
    result.program = required("program");
    result.channels = unsigned_integer<std::uint32_t>(required("channels"), "channels");
    if (!result.channels || result.channels > 1024) throw std::invalid_argument("--channels must be in [1, 1024]");
    if (values.contains("backend")) {
        const auto& value = values.at("backend");
        if (value == "auto") result.backend = payload::Backend::Auto;
        else if (value == "wordcode") result.backend = payload::Backend::Wordcode;
        else if (value == "zoned") result.backend = payload::Backend::Zoned;
        else if (value == "dense") result.backend = payload::Backend::Dense;
        else if (value == "scalar") result.backend = payload::Backend::Scalar;
        else throw std::invalid_argument("--backend must be auto, wordcode, zoned, dense, or scalar");
    }
    if (values.contains("payload-validation")) {
        const auto& value = values.at("payload-validation");
        if (value == "full") result.validation = payload::Validation::Full;
        else if (value == "on-demand") result.validation = payload::Validation::OnDemand;
        else throw std::invalid_argument("--payload-validation must be full or on-demand");
    }
    if (values.contains("first-row")) result.first = unsigned_integer<std::uint64_t>(values.at("first-row"), "first-row");
    if (values.contains("row-count")) result.count = unsigned_integer<std::uint64_t>(values.at("row-count"), "row-count");
    if (values.contains("max-output-bytes"))
        result.max_output_bytes = unsigned_integer<std::uint64_t>(values.at("max-output-bytes"), "max-output-bytes");
    if (values.contains("write-rows")) result.output_directory = fs::path(values.at("write-rows"));
    if (values.contains("json")) result.json_file = fs::path(values.at("json"));
    if (result.output_directory && result.json_file) {
        const auto directory = fs::weakly_canonical(fs::absolute(*result.output_directory));
        const auto destination = fs::weakly_canonical(fs::absolute(*result.json_file));
        const auto name = path_text(destination.filename());
        if (destination.parent_path() == directory && name.starts_with("row-") && name.ends_with(".bin"))
            throw std::invalid_argument("--json cannot use a reserved row-*.bin name inside --write-rows");
    }
    return result;
}

// Exclusive creation prevents replacing a pre-existing path, including a
// symlink. A failed write leaves its newly created file for explicit reporting.
class NewFile {
public:
    explicit NewFile(const fs::path& path) : path_(path) {
#ifdef _WIN32
        handle_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            throw std::system_error(static_cast<int>(error), std::system_category(), "create new file " + path_text(path));
        }
#else
        handle_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (handle_ < 0) {
            const auto error = errno;
            throw std::system_error(error, std::generic_category(), "create new file " + path_text(path));
        }
#endif
    }
    NewFile(const NewFile&) = delete;
    NewFile& operator=(const NewFile&) = delete;
    ~NewFile() { close_noexcept(); }

    void write(std::span<const std::uint8_t> bytes, std::uint64_t& written_bytes) {
        while (!bytes.empty()) {
            const auto chunk = std::min<std::size_t>(bytes.size(), 1U << 30);
#ifdef _WIN32
            DWORD written{};
            if (!WriteFile(handle_, bytes.data(), static_cast<DWORD>(chunk), &written, nullptr)) {
                const auto error = GetLastError();
                throw std::system_error(static_cast<int>(error), std::system_category(), "write " + path_text(path_));
            }
            if (written == 0) throw std::runtime_error("zero-byte write to " + path_text(path_));
#else
            const auto written = ::write(handle_, bytes.data(), chunk);
            if (written < 0 && errno == EINTR) continue;
            if (written < 0) {
                const auto error = errno;
                throw std::system_error(error, std::generic_category(), "write " + path_text(path_));
            }
            if (written == 0) throw std::runtime_error("zero-byte write to " + path_text(path_));
#endif
            written_bytes += static_cast<std::uint64_t>(written);
            bytes = bytes.subspan(static_cast<std::size_t>(written));
        }
    }

    void close() {
#ifdef _WIN32
        const auto handle = std::exchange(handle_, INVALID_HANDLE_VALUE);
        if (handle != INVALID_HANDLE_VALUE && !CloseHandle(handle)) {
            const auto error = GetLastError();
            throw std::system_error(static_cast<int>(error), std::system_category(), "close " + path_text(path_));
        }
#else
        // Retrying close after EINTR can close an unrelated reused descriptor.
        const auto handle = std::exchange(handle_, -1);
        if (handle >= 0 && ::close(handle) != 0) {
            const auto error = errno;
            throw std::system_error(error, std::generic_category(), "close " + path_text(path_));
        }
#endif
    }
private:
    void close_noexcept() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; }
#else
        if (handle_ >= 0) { ::close(handle_); handle_ = -1; }
#endif
    }
    fs::path path_;
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int handle_{-1};
#endif
};

class OutputSink final : public payload::RowSink {
public:
    OutputSink(std::shared_ptr<const payload::Snapshot> snapshot, std::uint64_t first,
               std::uint64_t count, std::optional<fs::path> directory)
        : snapshot_(std::move(snapshot)), first_(first), count_(count), directory_(std::move(directory)) {}

    void row(std::uint64_t global_row, std::span<const std::uint8_t> bytes) override {
        if (global_row < first_ || global_row - first_ >= count_ || (completed_rows && global_row <= last_row_))
            throw std::runtime_error("row sink received an out-of-order or out-of-range row");
        if (bytes.size() != snapshot_->decoded_size(global_row))
            throw std::runtime_error("row sink received an unexpected decoded length");
        if (directory_) {
            const auto path = *directory_ / ("row-" + std::to_string(global_row) + ".bin");
            NewFile file(path);
            ++created_files;
            incomplete_file = path;
            incomplete_file_bytes = 0;
            file.write(bytes, incomplete_file_bytes);
            file.close();
            incomplete_file.reset();
            incomplete_file_bytes = 0;
            ++completed_files;
        }
        // Consume the borrowed bytes before returning. Include row identity and
        // length so distinct row sequences cannot alias by concatenation alone.
        hash_integer(global_row);
        hash_integer(bytes.size());
        for (const auto byte : bytes) hash = (hash ^ byte) * fnv_prime;
        ++completed_rows;
        compressed_bytes += snapshot_->frame(global_row).size();
        decoded_bytes += bytes.size();
        last_row_ = global_row;
    }

    std::uint64_t completed_rows{}, compressed_bytes{}, decoded_bytes{};
    std::uint64_t created_files{}, completed_files{}, incomplete_file_bytes{};
    std::uint64_t hash{fnv_basis};
    std::optional<fs::path> incomplete_file;
private:
    void hash_integer(std::uint64_t value) noexcept {
        for (unsigned i = 0; i < 8; ++i) { hash = (hash ^ (value & 255U)) * fnv_prime; value >>= 8; }
    }
    std::shared_ptr<const payload::Snapshot> snapshot_;
    std::uint64_t first_{}, count_{}, last_row_{};
    std::optional<fs::path> directory_;
};

std::string read_program(const fs::path& path) {
    if (!fs::is_regular_file(path)) throw std::invalid_argument("program is not a regular file: " + path_text(path));
    const auto size = fs::file_size(path);
    if (size > 1024U * 1024U) throw std::invalid_argument("program file exceeds 1 MiB");
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot open program: " + path_text(path));
    std::string text(static_cast<std::size_t>(size), '\0');
    if (size && !input.read(text.data(), static_cast<std::streamsize>(size)))
        throw std::runtime_error("cannot read complete program: " + path_text(path));
    char extra{};
    if (input.get(extra)) throw std::runtime_error("program grew during read: " + path_text(path));
    if (!input.eof()) throw std::runtime_error("program read failed: " + path_text(path));
    return text;
}

struct PhaseTimer {
    explicit PhaseTimer(std::optional<double>& result) : result_(result), start_(Clock::now()) {}
    ~PhaseTimer() { result_ = std::chrono::duration<double, std::milli>(Clock::now() - start_).count(); }
    std::optional<double>& result_;
    Clock::time_point start_;
};

struct Report {
    std::optional<Options> options;
    std::shared_ptr<const payload::Snapshot> snapshot;
    std::unique_ptr<payload::Worker> worker;
    std::unique_ptr<OutputSink> sink;
    std::optional<payload::RunStats> run;
    std::optional<std::uint64_t> count;
    std::optional<double> admission_ms, preparation_ms, run_ms;
    std::string phase{"arguments"}, error, report_error;
    bool output_directory_created{}, json_file_created{}, json_file_attempted{};
};

std::string report_json(const Report& report) {
    const auto* options = report.options ? &*report.options : nullptr;
    const auto* stats = report.snapshot ? &report.snapshot->stats() : nullptr;
    const auto* run = report.run ? &*report.run : nullptr;
    const auto* sink = report.sink.get();
    const bool failed = !report.error.empty() || !report.report_error.empty();
    const auto numeric = [](const auto& value) { return value ? std::to_string(*value) : std::string("null"); };
    const auto file = [](const std::optional<fs::path>& path) { return path ? quote(path_text(*path)) : std::string("null"); };
    std::ostringstream hash;
    hash.imbue(std::locale::classic());
    hash << std::hex << std::setw(16) << std::setfill('0') << (sink ? sink->hash : fnv_basis);
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "{\"version\":\"0.13.0\",\"command\":\"run\",\"status\":" << quote(failed ? "error" : "ok")
        << ",\"phase\":" << quote(report.phase)
        << ",\"error\":" << (report.error.empty() ? "null" : quote(report.error))
        << ",\"report_error\":" << (report.report_error.empty() ? "null" : quote(report.report_error))
        << ",\"requested_backend\":" << (options ? quote(payload::backend_name(options->backend)) : "null")
        << ",\"backend\":" << (run ? quote(payload::backend_name(run->backend)) : "null")
        << ",\"payload_validation\":" << (options ? quote(options->validation == payload::Validation::Full ? "full" : "on-demand") : "null")
        << ",\"first_row\":" << (options ? std::to_string(options->first) : "null")
        << ",\"row_count\":" << numeric(report.count)
        << ",\"snapshot_rows\":" << (report.snapshot ? std::to_string(report.snapshot->rows()) : "null")
        << ",\"channels\":" << (options ? std::to_string(options->channels) : "null")
        << ",\"raw_file\":" << (options ? quote(path_text(options->raw)) : "null")
        << ",\"packed_file\":" << (options ? quote(path_text(options->packed)) : "null")
        << ",\"payload_file\":" << (options ? quote(path_text(options->payload_file)) : "null")
        << ",\"program_file\":" << (options ? quote(path_text(options->program)) : "null")
        << ",\"selected_rows\":" << (run ? std::to_string(run->selected_rows) : "null")
        << ",\"completed_rows\":" << (sink ? sink->completed_rows : 0)
        << ",\"byte_counts_scope\":\"completed_row_sink_callbacks\""
        << ",\"compressed_payload_bytes\":" << (sink ? sink->compressed_bytes : 0)
        << ",\"decoded_payload_bytes\":" << (sink ? sink->decoded_bytes : 0)
        << ",\"output_bytes\":" << (sink ? sink->decoded_bytes : 0)
        << ",\"feature_requested_bytes\":" << (run ? std::to_string(run->feature_requested_bytes) : "null")
        << ",\"zone_true_rows\":" << (run ? std::to_string(run->zone_true_rows) : "null")
        << ",\"zone_false_rows\":" << (run ? std::to_string(run->zone_false_rows) : "null")
        << ",\"zone_refined_rows\":" << (run ? std::to_string(run->zone_refined_rows) : "null")
        << ",\"raw_feature_bytes\":" << (stats ? std::to_string(stats->raw_feature_bytes) : "null")
        << ",\"packed_feature_bytes\":" << (stats ? std::to_string(stats->packed_feature_bytes) : "null")
        << ",\"payload_store_bytes\":" << (stats ? std::to_string(stats->payload_store_bytes) : "null")
        << ",\"block_range_bytes\":" << (stats ? std::to_string(stats->block_range_bytes) : "null")
        << ",\"snapshot_logical_bytes\":" << (stats ? std::to_string(stats->raw_feature_bytes + stats->packed_feature_bytes + stats->payload_store_bytes + stats->block_range_bytes) : "null")
        << ",\"snapshot_decoded_payload_bytes\":" << (stats ? std::to_string(stats->decoded_payload_bytes) : "null")
        << ",\"admission_decoded_payload_bytes\":" << (stats ? std::to_string(stats->all_payloads_verified ? stats->decoded_payload_bytes : 0) : "null")
        << ",\"worker_scratch_bytes_estimate\":" << (report.worker ? std::to_string(report.worker->scratch_bytes()) : "null")
        << ",\"all_payloads_verified\":" << (stats ? (stats->all_payloads_verified ? "true" : "false") : "null")
        << ",\"admission_ms\":" << numeric(report.admission_ms)
        << ",\"preparation_ms\":" << numeric(report.preparation_ms)
        << ",\"run_ms\":" << numeric(report.run_ms)
        << ",\"zstd_version\":" << quote(payload::zstd_version())
        << ",\"max_output_bytes\":" << (options ? std::to_string(options->max_output_bytes) : "null")
        << ",\"row_output_mode\":" << quote(options && options->output_directory ? "files" : "hash")
        << ",\"output_directory\":" << (options ? file(options->output_directory) : "null")
        << ",\"output_directory_created\":" << (report.output_directory_created ? "true" : "false")
        << ",\"created_row_files\":" << (sink ? sink->created_files : 0)
        << ",\"completed_row_files\":" << (sink ? sink->completed_files : 0)
        << ",\"row_output_complete\":" << (run ? "true" : "false")
        << ",\"partial_output\":" << (!run && sink && (sink->completed_rows || sink->created_files) ? "true" : "false")
        << ",\"incomplete_row_file\":" << (sink ? file(sink->incomplete_file) : "null")
        << ",\"incomplete_row_file_bytes\":" << (sink ? sink->incomplete_file_bytes : 0)
        << ",\"output_hash_algorithm\":\"fnv1a64_noncryptographic\""
        << ",\"output_hash_encoding\":\"for each completed row: u64le(global_row), u64le(length), bytes\""
        << ",\"output_hash\":" << quote(hash.str())
        << ",\"json_file\":" << (options ? file(options->json_file) : "null")
        << ",\"json_file_created\":" << (report.json_file_created ? "true" : "false") << '}';
    return out.str();
}

void usage() {
    std::cout << R"(suffice_payload 0.13.0 - exact ordered gate-to-payload execution

Usage:
  suffice_payload run --raw FEATURES.raw --packed FEATURES.suf --payload ROWS.sufpay
      --channels N --program QUERY.suf [options]

Options:
  --backend auto|wordcode|zoned|dense|scalar  Default: auto
  --first-row N                              Default: 0
  --row-count N                              Default: remaining rows
  --payload-validation full|on-demand        Default: full
  --max-output-bytes N                       Default: UINT64_MAX
  --write-rows NEWdirectory                  Write row-<global-row>.bin files
  --json NEWpath                            Also save the JSON report

The program is a .suf source file. Row ranges use global indices; count zero is
valid. Auto uses verified block bounds and wordcode when lowering is complete,
otherwise scalar. Full admission verifies every payload before querying;
on-demand verifies only selected payload contents, while admitting all metadata.

Without --write-rows, selected rows are consumed into a count and FNV-1a summary.
The hash includes each global row index and byte length as little-endian u64s,
then its bytes; it is a diagnostic checksum, not an integrity guarantee.
Output directories and report files must be new; their parents must exist.
Inside the row directory, row-*.bin names are reserved for decoded payloads.
Files are created exclusively and never overwritten. An error can leave earlier
rows and an incomplete current file; the JSON report identifies that partial
output. Writes are not a crash-durable transaction. Output-byte limits are checked
against the exact selection before the first row callback.

JSON goes to stdout on success or error; errors also go to stderr. Admission time
includes owned copies and the selected verification policy. Preparation includes
program reading, query compilation, and worker construction. Run time includes
selection, selected payload decoding, hashing, and optional row-file writes.
Snapshot logical bytes exclude temporary admission copies and allocator overhead;
worker scratch is an estimate, not process peak memory. Default admission limits:
512 MiB logical snapshot, 1,048,576 rows, 1 MiB per decoded row, 4 GiB decoded total.
)";
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && (std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "help"))
        || (argc == 3 && std::string_view(argv[1]) == "run" && std::string_view(argv[2]) == "--help")) {
        usage();
        return 0;
    }
    if (argc == 2 && (std::string_view(argv[1]) == "--version" || std::string_view(argv[1]) == "version")) {
        std::cout << "suffice_payload 0.13.0\n";
        return 0;
    }
    Report report;
    std::unique_ptr<NewFile> json_file;
    try {
        report.options = parse_options(argc, argv);
        const auto& options = *report.options;
        report.phase = "admission";
        {
            PhaseTimer timer(report.admission_ms);
            report.snapshot = payload::Snapshot::load(options.raw, options.channels,
                options.packed, options.payload_file, {}, options.validation);
        }
        report.phase = "preparation";
        if (options.first > report.snapshot->rows()) throw std::invalid_argument("--first-row is outside the snapshot");
        report.count = options.count.value_or(report.snapshot->rows() - options.first);
        if (*report.count > report.snapshot->rows() - options.first)
            throw std::invalid_argument("--row-count extends outside the snapshot");
        {
            PhaseTimer timer(report.preparation_ms);
            const auto program = suffice::parse_program(read_program(options.program));
            auto query = payload::Query::prepare(report.snapshot, program);
            report.worker = std::make_unique<payload::Worker>(std::move(query));
        }
        report.phase = "output_setup";
        if (options.output_directory) {
            if (!fs::create_directory(*options.output_directory))
                throw std::invalid_argument("refusing existing output directory: " + path_text(*options.output_directory));
            report.output_directory_created = true;
        }
        if (options.json_file) {
            report.json_file_attempted = true;
            json_file = std::make_unique<NewFile>(*options.json_file);
            report.json_file_created = true;
        }
        report.sink = std::make_unique<OutputSink>(report.snapshot, options.first, *report.count, options.output_directory);
        report.phase = "run";
        {
            PhaseTimer timer(report.run_ms);
            report.run = report.worker->run(options.backend, options.first, *report.count, *report.sink,
                payload::OutputLimits{UINT64_MAX, options.max_output_bytes});
        }
        report.phase = "complete";
    } catch (const std::exception& error) {
        report.error = error.what();
    }

    // Save admission/preparation errors too, when the requested report path is
    // usable. Do not retry a creation that already failed during output setup.
    if (!report.json_file_attempted && report.options && report.options->json_file) {
        report.json_file_attempted = true;
        try {
            json_file = std::make_unique<NewFile>(*report.options->json_file);
            report.json_file_created = true;
        } catch (const std::exception& error) {
            report.report_error = std::string("could not create requested JSON report: ") + error.what();
        }
    }
    auto json = report_json(report) + '\n';
    if (json_file) {
        try {
            std::uint64_t written{};
            json_file->write({reinterpret_cast<const std::uint8_t*>(json.data()), json.size()}, written);
            json_file->close();
        } catch (const std::exception& error) {
            report.report_error = std::string(error.what()) + "; the newly created JSON file may be incomplete";
            report.phase = "reporting";
            json = report_json(report) + '\n';
        }
    }
    std::cout << json;
    std::cout.flush();
    if (!report.error.empty()) std::cerr << "suffice_payload: " << report.error << '\n';
    if (!report.report_error.empty()) std::cerr << "suffice_payload: " << report.report_error << '\n';
    if (!report.run && report.sink && (report.sink->completed_rows || report.sink->created_files))
        std::cerr << "suffice_payload: partial output retained; inspect completed_rows and incomplete_row_file in the JSON report\n";
    if (!std::cout) {
        std::cerr << "suffice_payload: could not write the JSON report to stdout\n";
        return 1;
    }
    return report.error.empty() && report.report_error.empty() ? 0 : 1;
}
