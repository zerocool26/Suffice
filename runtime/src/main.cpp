#include "suffice/ir.hpp"
#include "suffice/runtime.hpp"
#include "suffice/storage.hpp"
#include "suffice/adaptive.hpp"
#include "suffice/codegen.hpp"
#include "suffice/portfolio_codegen.hpp"
#include "suffice/benchmark_order.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

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
using Clock = std::chrono::steady_clock;
using suffice::i64;

namespace {

std::string path_text(const fs::path& path) {
    const auto u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
}

std::string quote(std::string_view text) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : text) {
        switch (c) {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
                else out << char(c);
        }
    }
    out << '"';
    return out.str();
}

double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::string number(double value) {
    if (!std::isfinite(value)) return "null";
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return out.str();
}

std::string ratio(double numerator, double denominator) {
    return denominator > 0 ? number(numerator / denominator) : "null";
}

template <class T>
T integer(std::string_view text, std::string_view name) {
    T result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid integer for --" + std::string(name) + ": " + std::string(text));
    return result;
}

struct Options {
    std::map<std::string, std::string> values;
    std::set<std::string> switches;

    Options(int argc, char** argv, std::initializer_list<std::string_view> value_names,
            std::initializer_list<std::string_view> switch_names = {}) {
        const std::set<std::string_view> allowed_values(value_names), allowed_switches(switch_names);
        for (int index = 2; index < argc; ++index) {
            const std::string argument = argv[index];
            if (!argument.starts_with("--")) throw std::invalid_argument("unexpected positional argument: " + argument);
            const std::string name = argument.substr(2);
            if (values.contains(name) || switches.contains(name)) throw std::invalid_argument("duplicate option: " + argument);
            if (allowed_switches.contains(name)) { switches.insert(name); continue; }
            if (!allowed_values.contains(name)) throw std::invalid_argument("unknown option for this command: " + argument);
            if (++index >= argc || std::string_view(argv[index]).starts_with("--"))
                throw std::invalid_argument("missing value for " + argument);
            values.emplace(name, argv[index]);
        }
    }
    bool has(std::string_view name) const { return values.contains(std::string(name)) || switches.contains(std::string(name)); }
    std::string get(std::string_view name, std::string_view fallback = {}) const {
        const auto found = values.find(std::string(name));
        return found == values.end() ? std::string(fallback) : found->second;
    }
    std::string required(std::string_view name) const {
        const auto value = get(name);
        if (value.empty()) throw std::invalid_argument("required option: --" + std::string(name));
        return value;
    }
    template <class T> T get_integer(std::string_view name, T fallback) const {
        return has(name) ? integer<T>(get(name), name) : fallback;
    }
};

void ensure_new_output(const fs::path& path) {
    if (path.empty()) throw std::invalid_argument("output path cannot be empty");
    if (fs::exists(path)) throw std::invalid_argument("refusing to overwrite existing output: " + path_text(path));
}

// Exclusive temporary creation and non-replacing installation on both hosts.
class NewFile {
public:
    explicit NewFile(fs::path destination) : destination_(std::move(destination)) {
        ensure_new_output(destination_);
        if (!destination_.parent_path().empty()) fs::create_directories(destination_.parent_path());
        static std::atomic<std::uint64_t> serial{};
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            temporary_ = destination_;
            temporary_ += ".tmp." + std::to_string(Clock::now().time_since_epoch().count()) + "." + std::to_string(serial++);
#ifdef _WIN32
            handle_ = CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) return;
            if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
                throw std::system_error(int(GetLastError()), std::system_category(), "create output temporary");
#else
            handle_ = ::open(temporary_.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
            if (handle_ >= 0) return;
            if (errno != EEXIST) throw std::system_error(errno, std::generic_category(), "create output temporary");
#endif
        }
        throw std::runtime_error("cannot obtain an exclusive temporary output");
    }
    NewFile(const NewFile&) = delete;
    ~NewFile() {
        close_noexcept();
        std::error_code ignored;
        if (!temporary_.empty()) fs::remove(temporary_, ignored);
    }
    void write(std::span<const std::uint8_t> bytes) {
        while (!bytes.empty()) {
#ifdef _WIN32
            DWORD written{};
            const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size(), 1u << 30));
            if (!WriteFile(handle_, bytes.data(), count, &written, nullptr) || written == 0)
                throw std::system_error(int(GetLastError()), std::system_category(), "write output");
            bytes = bytes.subspan(written);
#else
            const auto count = std::min<std::size_t>(bytes.size(), 1u << 30);
            const auto written = ::write(handle_, bytes.data(), count);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::system_error(errno, std::generic_category(), "write output");
            bytes = bytes.subspan(static_cast<std::size_t>(written));
#endif
        }
    }
    void commit() {
#ifdef _WIN32
        if (!FlushFileBuffers(handle_)) throw std::system_error(int(GetLastError()), std::system_category(), "flush output");
        close_noexcept();
        if (!MoveFileExW(temporary_.c_str(), destination_.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::system_error(int(GetLastError()), std::system_category(), "install output without replacement");
#else
        if (::fsync(handle_) != 0) throw std::system_error(errno, std::generic_category(), "flush output");
        close_noexcept();
        if (::link(temporary_.c_str(), destination_.c_str()) != 0)
            throw std::system_error(errno, std::generic_category(), "install output without replacement");
        // After linking, the completed destination is valid even if cleanup fails.
        if (::unlink(temporary_.c_str()) != 0) return;
#endif
        temporary_.clear();
    }
private:
    void close_noexcept() noexcept {
#ifdef _WIN32
        if (handle_ != INVALID_HANDLE_VALUE) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; }
#else
        if (handle_ >= 0) { ::close(handle_); handle_ = -1; }
#endif
    }
    fs::path destination_, temporary_;
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    int handle_{-1};
#endif
};

void write_text(const fs::path& path, const std::string& text) {
    NewFile output(path);
    output.write({reinterpret_cast<const std::uint8_t*>(text.data()), text.size()});
    output.commit();
}

void emit_json(const std::string& json, const Options& options) {
    if (options.has("json")) write_text(fs::path(options.get("json")), json + "\n");
    std::cout << json << '\n';
}

std::uint64_t random64(std::uint64_t& state) noexcept {
    auto value = (state += 0x9e3779b97f4a7c15ull);
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

std::int32_t generated(std::string_view pattern, std::uint64_t row,
                       std::uint32_t channel, std::uint64_t seed, std::uint64_t& random_state) {
    const auto r = random64(random_state);
    if (pattern == "uniform") return std::bit_cast<std::int32_t>(static_cast<std::uint32_t>(r));
    if (pattern == "constant") return static_cast<std::int32_t>(12345 + channel);
    if (pattern == "boundary") {
        if (row % 4096 == 0) return (r & 1) ? 100001 : -100001;
        return static_cast<std::int32_t>(r % 33) - 16;
    }
    if (pattern == "alternating") {
        const i64 base = ((row + channel) & 1) ? 1000000000 : -1000000000;
        return static_cast<std::int32_t>(base + static_cast<i64>(r % 8191) - 4095);
    }
    if (pattern == "smooth") {
        const i64 phase = static_cast<i64>((row + std::uint64_t(channel) * 67) % 8192);
        const i64 triangle = phase <= 4096 ? phase : 8192 - phase;
        return static_cast<std::int32_t>((triangle - 2048) * 400000);
    }
    // Cluster identity depends on row position, not the generator's call order.
    std::uint64_t cluster_state = seed ^ ((row / 256) * 0x9e3779b97f4a7c15ull) ^ channel;
    const auto cluster = random64(cluster_state);
    if ((cluster & 31u) == 0) return static_cast<std::int32_t>(r % 200001) - 100000;
    const i64 base = (cluster & 32u) ? (i64{1} << 29) : -(i64{1} << 29);
    return static_cast<std::int32_t>(base + static_cast<i64>(r % 4095) - 2047);
}

std::string info_json(const suffice::DatasetInfo& info) {
    std::ostringstream out;
    out << "{\"rows\":" << info.rows << ",\"channels\":" << info.channels
        << ",\"block_rows\":" << info.block_rows << ",\"metadata_bytes\":" << info.metadata_bytes
        << ",\"file_bytes\":" << info.file_bytes << ",\"channel_ranges\":[";
    for (std::size_t index = 0; index < info.ranges.size(); ++index) {
        if (index) out << ',';
        out << "{\"min\":" << info.ranges[index].min << ",\"max\":" << info.ranges[index].max << '}';
    }
    out << "]}";
    return out.str();
}

std::string stats_json(const suffice::ExecutionStats& stats) {
    std::ostringstream out;
    out << "{\"kernel\":" << quote(stats.kernel) << ",\"rows\":" << stats.rows
        << ",\"accepted\":" << stats.accepted << ",\"requested_payload_bytes\":" << stats.requested_payload_bytes
        << ",\"estimated_cacheline_bytes\":" << stats.estimated_cacheline_bytes
        << ",\"interval_evaluations\":" << stats.interval_evaluations
        << ",\"exact_evaluations\":" << stats.exact_evaluations << ",\"region_loads\":" << stats.region_loads
        << ",\"cache_reuses\":" << stats.cache_reuses << ",\"loaded_planes\":" << stats.loaded_planes
        << ",\"bulk_resolutions\":" << stats.bulk_resolutions << ",\"refined_rows\":" << stats.refined_rows
        << ",\"frontier_prefix_resolved_rows\":" << stats.frontier_prefix_resolved_rows
        << ",\"frontier_fallback_words\":" << stats.frontier_fallback_words
        << ",\"frontier_dirty_words\":" << stats.frontier_dirty_words
        << ",\"frontier_fallback_rows\":" << stats.frontier_fallback_rows
        << ",\"frontier_dense_tiles\":" << stats.frontier_dense_tiles
        << ",\"frontier_sparse_words\":" << stats.frontier_sparse_words
        << ",\"refinement_fraction\":" << (stats.rows ? number(double(stats.refined_rows) / double(stats.rows)) : "0")
        << ",\"peak_scratch_bytes\":" << stats.peak_scratch_bytes << ",\"output_bytes\":" << stats.output_bytes
        << ",\"resolved_by_bits\":[";
    for (std::size_t index = 0; index < stats.resolved_by_bits.size(); ++index) {
        if (index) out << ',';
        out << stats.resolved_by_bits[index];
    }
    out << "]}";
    return out.str();
}

suffice::ExecutionOptions execution_options(const Options& options) {
    suffice::ExecutionOptions result;
    result.specialize = !options.has("no-specialize");
    if (options.has("stages")) {
        result.stages.clear();
        const auto text = options.get("stages");
        std::size_t start = 0;
        do {
            const auto end = text.find(',', start);
            const auto part = std::string_view(text).substr(start, end == std::string::npos ? text.size() - start : end - start);
            const auto bits = integer<unsigned>(part, "stages");
            if (!bits || bits > 32 || (!result.stages.empty() && bits <= result.stages.back()))
                throw std::invalid_argument("--stages must be strictly increasing within 1..32");
            result.stages.push_back(bits);
            if (end == std::string::npos) break;
            start = end + 1;
        } while (start <= text.size());
        if (result.stages.empty() || result.stages.back() != 32)
            throw std::invalid_argument("--stages must finish at 32");
    }
    return result;
}

suffice::AdaptiveOptions adaptive_options(const Options& options) {
    suffice::AdaptiveOptions result;
    result.backend = suffice::parse_adaptive_backend(options.get("backend", "auto"));
    result.rounds = options.get_integer<unsigned>("calibration-repeat", 3);
    if (!result.rounds || result.rounds > 9) throw std::invalid_argument("--calibration-repeat must be in 1..9");
    result.tile_rows = options.get_integer<std::size_t>("tile-rows", 256);
    result.frontier_prefix_bits = options.get_integer<unsigned>("frontier-prefix-bits", 12);
    if (result.frontier_prefix_bits > 32) throw std::invalid_argument("--frontier-prefix-bits must be in 0..32");
    result.frontier_repair.policy = suffice::parse_frontier_repair_policy(options.get("frontier-repair", "hybrid"));
    result.frontier_repair.dense_threshold_percent = options.get_integer<unsigned>("frontier-dense-percent", 25);
    if (!result.frontier_repair.dense_threshold_percent || result.frontier_repair.dense_threshold_percent > 100)
        throw std::invalid_argument("--frontier-dense-percent must be in 1..100");
    if (!result.tile_rows || result.tile_rows > 65536) throw std::invalid_argument("--tile-rows must be in 1..65536");
    const auto mib = options.get_integer<std::uint64_t>("max-scratch-mib", 128);
    if (!mib || mib > std::numeric_limits<std::uint64_t>::max() / (1ull << 20))
        throw std::invalid_argument("--max-scratch-mib must be positive and fit a uint64 byte count");
    result.max_candidate_scratch_bytes = mib * (1ull << 20);
    result.calibration = suffice::parse_calibration_mode(options.get("calibration", "sampled"));
    result.sample_policy = suffice::parse_sampling_policy(options.get("sample-policy", "representative"));
    result.sample_rows = options.get_integer<std::uint64_t>("sample-rows", 32768);
    result.sample_windows = options.get_integer<unsigned>("sample-windows", 8);
    result.sample_seed = options.get_integer<std::uint64_t>("sample-seed", result.sample_seed);
    result.max_calibration_row_evaluations = options.get_integer<std::uint64_t>("calibration-work-rows", 1ull << 20);
    result.calibration_time_budget_ms = static_cast<double>(options.get_integer<std::uint64_t>("calibration-ms", 25));
    const auto total_mib = options.get_integer<std::uint64_t>("max-calibration-mib", 128);
    if (!total_mib || total_mib > std::numeric_limits<std::uint64_t>::max() / (1ull << 20))
        throw std::invalid_argument("--max-calibration-mib must be positive and fit uint64 bytes");
    result.max_calibration_workspace_bytes = total_mib * (1ull << 20);
    result.successive_halving = !options.has("no-race");
    result.execution = execution_options(options);
    return result;
}

std::string samples_json(std::span<const double> values) {
    std::ostringstream out;
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) { if (i) out << ','; out << number(values[i]); }
    out << ']';
    return out.str();
}

std::string adaptive_report_json(const suffice::AdaptiveReport& report) {
    std::ostringstream out;
    out << "{\"selected\":" << quote(suffice::adaptive_backend_name(report.selected))
        << ",\"forced\":" << (report.forced ? "true" : "false")
        << ",\"rounds\":" << report.rounds << ",\"calibrated_rows\":" << report.calibrated_rows
        << ",\"requested_rounds\":" << report.requested_rounds
        << ",\"calibration_mode\":" << quote(suffice::calibration_mode_name(report.calibration))
        << ",\"sample_policy\":" << quote(suffice::sampling_policy_name(report.sample_policy))
        << ",\"dataset_rows\":" << report.dataset_rows
        << ",\"reference_is_full\":" << (report.reference_is_full ? "true" : "false")
        << ",\"reference_scope\":" << quote(report.reference_is_full ? "complete-dataset" : "concatenated-local-window-masks")
        << ",\"race_enabled\":" << (report.race_enabled ? "true" : "false")
        << ",\"minimum_rounds_before_elimination\":" << report.minimum_rounds_before_elimination
        << ",\"time_budget_reached\":" << (report.time_budget_reached ? "true" : "false")
        << ",\"calibration_time_budget_ms\":" << number(report.calibration_time_budget_ms)
        << ",\"sample_seed\":" << report.sample_seed
        << ",\"sample_alignment_rows\":" << report.sample_alignment_rows
        << ",\"sample_population_rows\":" << report.sample_population_rows
        << ",\"sample_unrepresented_rows\":" << report.sample_unrepresented_rows
        << ",\"boundary_probe_rows\":" << report.boundary_probe_rows
        << ",\"calibration_row_budget\":" << report.calibration_row_budget
        << ",\"oracle_row_evaluations\":" << report.oracle_row_evaluations
        << ",\"warmup_row_evaluations\":" << report.warmup_row_evaluations
        << ",\"timed_row_evaluations\":" << report.timed_row_evaluations
        << ",\"boundary_oracle_row_evaluations\":" << report.boundary_oracle_row_evaluations
        << ",\"boundary_candidate_row_evaluations\":" << report.boundary_candidate_row_evaluations
        << ",\"total_calibration_row_evaluations\":" << report.total_calibration_row_evaluations
        << ",\"max_calibration_workspace_bytes\":" << report.max_calibration_workspace_bytes
        << ",\"validation_performed\":" << (report.validation_performed ? "true" : "false")
        << ",\"validation_ms\":" << number(report.validation_ms)
        << ",\"compile_prepare_ms\":" << number(report.compile_prepare_ms)
        << ",\"oracle_ms\":" << number(report.oracle_ms)
        << ",\"warmup_ms\":" << number(report.warmup_ms)
        << ",\"calibration_ms\":" << number(report.calibration_ms)
        << ",\"total_prepare_ms\":" << number(report.total_prepare_ms)
        << ",\"tie_fraction\":" << number(report.tie_fraction)
        << ",\"calibration_workspace_bytes\":" << report.calibration_workspace_bytes
        << ",\"selected_scratch_bytes\":" << report.selected_scratch_bytes
        << ",\"max_candidate_scratch_bytes\":" << report.max_candidate_scratch_bytes
        << ",\"reference_checksum\":" << report.reference_checksum
        << ",\"reference_accepted\":" << report.reference_accepted << ",\"windows\":[";
    for (std::size_t i = 0; i < report.windows.size(); ++i) {
        const auto& w = report.windows[i];
        if (i) out << ',';
        out << "{\"first_row\":" << w.first_row << ",\"row_count\":" << w.row_count
            << ",\"mask_word_offset\":" << w.mask_word_offset
            << ",\"population_rows\":" << w.population_rows
            << ",\"timing_weight\":" << number(w.timing_weight) << '}';
    }
    out << "],\"boundary_probes\":[";
    for (std::size_t i = 0; i < report.boundary_probes.size(); ++i) {
        const auto& w = report.boundary_probes[i];
        if (i) out << ',';
        out << "{\"first_row\":" << w.first_row << ",\"row_count\":" << w.row_count
            << ",\"mask_word_offset\":" << w.mask_word_offset
            << ",\"population_rows\":" << w.population_rows
            << ",\"timing_weight\":" << number(w.timing_weight) << '}';
    }
    out << "],\"candidates\":[";
    for (std::size_t i = 0; i < report.candidates.size(); ++i) {
        const auto& c = report.candidates[i];
        if (i) out << ',';
        out << "{\"backend\":" << quote(suffice::adaptive_backend_name(c.backend))
            << ",\"eligible\":" << (c.eligible ? "true" : "false")
            << ",\"verified\":" << (c.verified ? "true" : "false")
            << ",\"selected\":" << (c.selected ? "true" : "false")
            << ",\"reason\":" << quote(c.reason) << ",\"description\":" << quote(c.description)
            << ",\"kernel\":" << quote(c.kernel) << ",\"samples_ms\":" << samples_json(c.samples_ms)
            << ",\"window_samples_ms\":" << samples_json(c.window_samples_ms)
            << ",\"eliminated\":" << (c.eliminated ? "true" : "false")
            << ",\"eliminated_after_round\":" << c.eliminated_after_round
            << ",\"warmup_row_evaluations\":" << c.warmup_row_evaluations
            << ",\"timed_row_evaluations\":" << c.timed_row_evaluations
            << ",\"mask_checks\":" << c.mask_checks
            << ",\"boundary_probe_row_evaluations\":" << c.boundary_probe_row_evaluations
            << ",\"boundary_mask_checks\":" << c.boundary_mask_checks
            << ",\"median_ms\":" << number(c.median_ms) << ",\"min_ms\":" << number(c.min_ms)
            << ",\"max_ms\":" << number(c.max_ms)
            << ",\"estimated_scratch_bytes\":" << c.estimated_scratch_bytes
            << ",\"scratch_bytes\":" << c.scratch_bytes << '}';
    }
    out << "]}";
    return out.str();
}

struct SelectedProgram {
    suffice::CompiledProgram plan;
    std::string specification;
    std::string builtin;
};

SelectedProgram select_program(const Options& options, const suffice::DatasetInfo& info) {
    SelectedProgram selected;
    selected.specification = options.get("program", "fir3");
    std::vector<suffice::Bounds> ranges;
    ranges.reserve(info.ranges.size());
    for (const auto range : info.ranges) ranges.push_back({range.min, range.max});
    const bool builtin = selected.specification == "identity" || selected.specification == "delta"
        || selected.specification == "fir3" || selected.specification == "energy";
    if (builtin) {
        selected.builtin = selected.specification;
        const i64 default_threshold = selected.builtin == "identity" ? 0 : selected.builtin == "energy" ? 1000000 : 100000;
        const auto threshold = options.get_integer<i64>("threshold", default_threshold);
        selected.plan = suffice::builtin_program(selected.builtin, threshold).compile(ranges);
    } else {
        if (options.has("threshold")) throw std::invalid_argument("a .suf program defines its threshold; --threshold applies only to a builtin");
        const fs::path path(selected.specification);
        const auto size = fs::file_size(path);
        if (size > 1024 * 1024) throw std::invalid_argument("program file exceeds 1 MiB");
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot open program: " + path_text(path));
        std::string text(static_cast<std::size_t>(size), '\0');
        input.read(text.data(), static_cast<std::streamsize>(size));
        if (!input && size) throw std::runtime_error("cannot read complete program: " + path_text(path));
        selected.plan = suffice::parse_program(text).compile(ranges);
    }
    return selected;
}

std::string stages_json(std::span<const unsigned> stages) {
    std::ostringstream out;
    out << '[';
    for (std::size_t index = 0; index < stages.size(); ++index) { if (index) out << ','; out << stages[index]; }
    out << ']';
    return out.str();
}

std::string machine_json() {
    std::string compiler;
#if defined(__clang__)
    compiler = "Clang " __clang_version__;
#elif defined(__GNUC__)
    compiler = "GCC " __VERSION__;
#elif defined(_MSC_VER)
    compiler = "MSVC " + std::to_string(_MSC_VER);
#else
    compiler = "unknown";
#endif
#ifdef _WIN32
    const char* os = "Windows";
#elif defined(__linux__)
    const char* os = "Linux";
#elif defined(__APPLE__)
    const char* os = "macOS";
#else
    const char* os = "other";
#endif
#ifdef NDEBUG
    const char* mode = "release";
#else
    const char* mode = "assertions-enabled";
#endif
    return "{\"os\":" + quote(os) + ",\"compiler\":" + quote(compiler)
        + ",\"build_mode\":" + quote(mode) + ",\"logical_cpus\":" + std::to_string(std::thread::hardware_concurrency())
        + ",\"avx2_available\":" + (suffice::avx2_available() ? "true" : "false") + '}';
}

constexpr std::string_view measurement_notes =
    "Requested payload counters describe software byte requests, not measured disk, DRAM, or PCIe traffic. "
    "Cacheline bytes are a 64-byte transaction model without a cache simulation. "
    "Frontier dirty words count uncertainty; fallback words/rows count physical repair, including resolved lanes in dense tiles. "
    "Scratch excludes mapped file storage and output masks. No GPU execution is performed.";

void command_generate(const Options& options) {
    const fs::path output_path(options.required("output"));
    const auto rows = options.get_integer<std::uint64_t>("rows", 1048576);
    const auto channels = options.get_integer<std::uint32_t>("channels", 1);
    const auto seed = options.get_integer<std::uint64_t>("seed", 20261110);
    const auto pattern = options.get("pattern", "clustered");
    if (!channels || channels > 1024) throw std::invalid_argument("--channels must be in 1..1024");
    if (rows > (std::uint64_t{1} << 40) / (4 * std::uint64_t(channels)))
        throw std::invalid_argument("generated file would exceed the 1 TiB command limit");
    const std::set<std::string_view> patterns{"clustered", "uniform", "boundary", "smooth", "alternating", "constant", "heterogeneous"};
    if (!patterns.contains(pattern)) throw std::invalid_argument("unknown generator pattern: " + pattern);
    if (options.has("json")) ensure_new_output(fs::path(options.get("json")));
    const auto start = Clock::now();
    NewFile output(output_path);
    std::array<std::uint8_t, 65536> buffer{};
    std::size_t used = 0;
    auto random_state = seed;
    for (std::uint64_t row = 0; row < rows; ++row) {
        std::string_view row_pattern=pattern;
        if(pattern=="heterogeneous") {
            constexpr std::array<std::string_view,4> quarters{{"clustered","uniform","boundary","smooth"}};
            row_pattern=quarters[static_cast<std::size_t>(std::min<std::uint64_t>(3,row*4/rows))];
        }
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            const auto value = std::bit_cast<std::uint32_t>(generated(row_pattern, row, channel, seed, random_state));
            for (unsigned byte = 0; byte < 4; ++byte) buffer[used++] = static_cast<std::uint8_t>(value >> (byte * 8));
            if (used == buffer.size()) { output.write(buffer); used = 0; }
        }
    }
    output.write(std::span<const std::uint8_t>(buffer.data(), used));
    output.commit();
    emit_json("{\"command\":\"generate\",\"output\":" + quote(path_text(output_path))
        + ",\"rows\":" + std::to_string(rows) + ",\"channels\":" + std::to_string(channels)
        + ",\"pattern\":" + quote(pattern) + ",\"seed\":" + std::to_string(seed)
        + ",\"bytes\":" + std::to_string(rows * channels * 4) + ",\"elapsed_ms\":" + number(elapsed_ms(start)) + '}', options);
}

void command_pack(const Options& options) {
    const fs::path input(options.required("input")), output(options.required("output"));
    const auto channels = options.get_integer<std::uint32_t>("channels", 1);
    const auto block_rows = options.get_integer<std::uint32_t>("block-rows", 64);
    if (options.has("json")) ensure_new_output(fs::path(options.get("json")));
    const auto start = Clock::now();
    const auto info = suffice::pack_file(input, output, channels, block_rows);
    const auto packing_ms = elapsed_ms(start);
    double verification_ms = 0;
    if (options.has("verify")) {
        const auto verify_start = Clock::now();
        suffice::PackedDataset(output).verify_payload();
        verification_ms = elapsed_ms(verify_start);
    }
    emit_json("{\"command\":\"pack\",\"dataset\":" + info_json(info)
        + ",\"packing_ms\":" + number(packing_ms) + ",\"verification_ms\":" + number(verification_ms)
        + ",\"payload_verified\":" + (options.has("verify") ? "true" : "false") + '}', options);
}

void command_inspect(const Options& options) {
    if (options.has("json")) ensure_new_output(fs::path(options.get("json")));
    const suffice::PackedDataset data(fs::path(options.required("input")));
    if (options.has("verify")) data.verify_payload();
    emit_json("{\"command\":\"inspect\",\"dataset\":" + info_json(data.info())
        + ",\"payload_verified\":" + (options.has("verify") ? "true" : "false") + '}', options);
}

void write_mask(const fs::path& path, std::span<const std::uint64_t> mask) {
    NewFile output(path);
    std::array<std::uint8_t, 8192> buffer{};
    std::size_t used = 0;
    for (const auto word : mask) {
        for (unsigned byte = 0; byte < 8; ++byte) buffer[used++] = static_cast<std::uint8_t>(word >> (8 * byte));
        if (used == buffer.size()) { output.write(buffer); used = 0; }
    }
    output.write(std::span<const std::uint8_t>(buffer.data(), used));
    output.commit();
}

void command_run(const Options& options) {
    if (options.has("json")) ensure_new_output(fs::path(options.get("json")));
    if (options.has("mask")) ensure_new_output(fs::path(options.get("mask")));
    const auto total_start = Clock::now();
    const auto adaptive = adaptive_options(options);
    const auto mapping_start = Clock::now();
    const suffice::PackedDataset data(fs::path(options.required("input")));
    const auto first_row = options.get_integer<std::uint64_t>("first-row", 0);
    if (first_row > data.info().rows) throw std::invalid_argument("--first-row outside dataset");
    const auto row_count = options.get_integer<std::uint64_t>("row-count", data.info().rows - first_row);
    if (row_count > data.info().rows - first_row) throw std::invalid_argument("--row-count outside remaining dataset");
    std::unique_ptr<suffice::RawDataset> raw;
    if (options.has("raw")) raw = std::make_unique<suffice::RawDataset>(fs::path(options.get("raw")), data.info().channels);
    const auto mapping_ms = elapsed_ms(mapping_start);
    const auto compile_start = Clock::now();
    const auto selected = select_program(options, data.info());
    const auto program_compile_ms = elapsed_ms(compile_start);
    suffice::AdaptiveExecutor executor(data, selected.plan, raw.get(), selected.builtin, adaptive);
    std::vector<std::uint64_t> mask(suffice::mask_words(row_count));
    const auto preparation_ms = elapsed_ms(total_start);
    const auto start = Clock::now();
    const auto stats = executor.run_range(first_row, row_count, mask);
    const auto execution_ms = elapsed_ms(start);
    const auto checksum = suffice::mask_checksum(mask);
    const auto mask_write_start = Clock::now();
    if (options.has("mask")) write_mask(fs::path(options.get("mask")), mask);
    const auto mask_write_ms = options.has("mask") ? elapsed_ms(mask_write_start) : 0;
    const auto total_ms = elapsed_ms(total_start);
    emit_json("{\"schema_version\":10,\"version\":\"0.13.0\",\"command\":\"run\",\"dataset\":" + info_json(data.info())
        + ",\"execution_range\":{\"first_row\":" + std::to_string(first_row) + ",\"row_count\":" + std::to_string(row_count) + "}"
        + ",\"program\":" + quote(selected.specification) + ",\"threshold\":" + std::to_string(selected.plan.threshold())
        + ",\"plan\":" + quote(selected.plan.describe()) + ",\"description\":" + quote(executor.description())
        + ",\"stages\":" + stages_json(adaptive.execution.stages)
        + ",\"raw_supplied\":" + (raw ? "true" : "false")
        + ",\"payload_verified\":true,\"raw_pair_verified\":" + (raw ? "true" : "false")
        + ",\"mapping_ms\":" + number(mapping_ms) + ",\"program_compile_ms\":" + number(program_compile_ms)
        + ",\"validation_ms\":" + number(executor.report().validation_ms)
        + ",\"preparation_ms\":" + number(preparation_ms) + ",\"execution_ms\":" + number(execution_ms)
        + ",\"mask_write_ms\":" + number(mask_write_ms) + ",\"total_before_json_ms\":" + number(total_ms)
        + ",\"selection\":" + adaptive_report_json(executor.report())
        + ",\"mask_checksum\":" + std::to_string(checksum) + ",\"stats\":" + stats_json(stats)
        + ",\"machine\":" + machine_json() + ",\"measurement_notes\":" + quote(measurement_notes) + '}', options);
}

suffice::CodegenOptions parse_codegen_options(const Options& options, bool portfolio = false) {
    suffice::CodegenOptions config;
    config.namespace_name = options.get("namespace", "suffice_generated");
    config.enable_avx2 = !options.has("scalar-only");
    config.lower_predicates = !options.has("original-ir");
    const auto strategy = options.get("encoded-strategy", "eager");
    if (strategy == "eager") config.encoded_strategy = suffice::EncodedStrategy::Eager;
    else if (strategy == "frontier") config.encoded_strategy = suffice::EncodedStrategy::Frontier;
    else if (strategy == "residual") config.encoded_strategy = suffice::EncodedStrategy::Residual;
    else if (strategy == "guarded-residual") config.encoded_strategy = suffice::EncodedStrategy::GuardedResidual;
    else throw std::invalid_argument("--encoded-strategy must be eager, frontier, residual, or guarded-residual");
    const auto placement = options.get("encoded-carry-placement", "eager");
    if (placement == "eager") config.encoded_carry_placement = suffice::EncodedCarryPlacement::Eager;
    else if (placement == "guarded") config.encoded_carry_placement = suffice::EncodedCarryPlacement::Guarded;
    else if (placement == "recompute") config.encoded_carry_placement = suffice::EncodedCarryPlacement::Recompute;
    else throw std::invalid_argument("--encoded-carry-placement must be eager, guarded, or recompute");
    const bool explicit_proof_budget = options.has("encoded-proof-nodes") || options.has("encoded-proof-steps") ||
                                      options.has("encoded-proof-attempts");
    if (explicit_proof_budget && !portfolio && config.encoded_strategy != suffice::EncodedStrategy::GuardedResidual)
        throw std::invalid_argument("encoded proof budgets require --encoded-strategy guarded-residual");
    config.guarded_residual_options.bdd_node_budget = options.get_integer<std::uint32_t>(
        "encoded-proof-nodes", config.guarded_residual_options.bdd_node_budget);
    config.guarded_residual_options.proof_step_budget = options.get_integer<std::uint64_t>(
        "encoded-proof-steps", config.guarded_residual_options.proof_step_budget);
    config.guarded_residual_options.rewrite_attempt_budget = options.get_integer<std::uint32_t>(
        "encoded-proof-attempts", config.guarded_residual_options.rewrite_attempt_budget);
    const auto schedule = options.get("encoded-schedule", "legacy");
    if (schedule == "legacy") config.encoded_schedule = suffice::EncodedSchedule::Legacy;
    else if (schedule == "pressure") config.encoded_schedule = suffice::EncodedSchedule::Pressure;
    else if (schedule == "bounds-first") config.encoded_schedule = suffice::EncodedSchedule::BoundsFirst;
    else throw std::invalid_argument("--encoded-schedule must be legacy, pressure, or bounds-first");
    const auto binding = options.get("encoded-plane-binding", "per-plane");
    if (binding == "per-plane") config.encoded_plane_binding = suffice::EncodedPlaneBinding::PerPlane;
    else if (binding == "grouped") config.encoded_plane_binding = suffice::EncodedPlaneBinding::Grouped;
    else throw std::invalid_argument("--encoded-plane-binding must be per-plane or grouped");
    const auto repair = options.get("encoded-repair-policy", "batch256");
    if (repair == "batch256") config.encoded_repair_policy = suffice::EncodedRepairPolicy::Batch256;
    else if (repair == "word64") config.encoded_repair_policy = suffice::EncodedRepairPolicy::Word64;
    else if (repair == "hybrid") config.encoded_repair_policy = suffice::EncodedRepairPolicy::Hybrid;
    else throw std::invalid_argument("--encoded-repair-policy must be batch256, word64, or hybrid");
    const auto layout = options.get("raw-layout", "interleaved");
    if (layout == "interleaved") config.raw_layout = suffice::RawLayout::Interleaved;
    else if (layout == "planar") config.raw_layout = suffice::RawLayout::Planar;
    else throw std::invalid_argument("--raw-layout must be interleaved or planar");
    config.encoded_prefix_bits = options.get_integer<unsigned>("encoded-prefix-bits", 12);
    if (config.encoded_prefix_bits > 32) throw std::invalid_argument("--encoded-prefix-bits must be in 0..32");
    return config;
}

// Shared by single-kernel and portfolio reports; every variant exposes its
// own proof scope and source-graph counters rather than a portfolio aggregate.
std::string generated_kernel_json_fields(const suffice::GeneratedKernel& generated,
                                       const suffice::CodegenOptions& config) {
    return std::string(",\"description\":") + quote(generated.description)
        + ",\"predicate_lowered\":" + (generated.predicate_lowered ? "true" : "false")
        + ",\"avx2_emitted\":" + (generated.avx2_emitted ? "true" : "false")
        + ",\"encoded_emitted\":" + (generated.encoded_emitted ? "true" : "false")
        + ",\"encoded_avx2_emitted\":" + (generated.encoded_avx2_emitted ? "true" : "false")
        + ",\"encoded_plane_binding\":" + quote(config.encoded_plane_binding == suffice::EncodedPlaneBinding::Grouped ? "grouped" : "per-plane")
        + ",\"encoded_grouped_binding_emitted\":" + (generated.encoded_grouped_binding_emitted ? "true" : "false")
        + ",\"encoded_repair_policy\":" + quote(config.encoded_repair_policy == suffice::EncodedRepairPolicy::Word64 ? "word64" :
            config.encoded_repair_policy == suffice::EncodedRepairPolicy::Hybrid ? "hybrid" : "batch256")
        + ",\"encoded_narrow_repair_emitted\":" + (generated.encoded_narrow_repair_emitted ? "true" : "false")
        + ",\"encoded_word_repair_max_dirty\":" + std::to_string(config.encoded_repair_policy == suffice::EncodedRepairPolicy::Word64 ? 4 :
            config.encoded_repair_policy == suffice::EncodedRepairPolicy::Hybrid ? 1 : 0)
        + ",\"encoded_plane_count\":" + std::to_string(generated.encoded_plane_count)
        + ",\"encoded_binding_groups\":" + std::to_string(generated.encoded_binding_groups)
        + ",\"encoded_binding_calls_per_block\":" + std::to_string(generated.encoded_binding_calls_per_block)
        + ",\"encoded_frontier_emitted\":" + (generated.encoded_frontier_emitted ? "true" : "false")
        + ",\"encoded_residual_emitted\":" + (generated.encoded_residual_emitted ? "true" : "false")
        + ",\"encoded_guarded_residual_emitted\":" + (generated.encoded_guarded_residual_emitted ? "true" : "false")
        + ",\"encoded_guarded_carry_emitted\":" + (generated.encoded_guarded_carry_emitted ? "true" : "false")
        + ",\"encoded_recompute_carry_emitted\":" + (generated.encoded_recompute_carry_emitted ? "true" : "false")
        + ",\"encoded_residual_exact_all_lanes\":" + (generated.encoded_residual_exact_all_lanes ? "true" : "false")
        + ",\"encoded_residual_scope\":" + quote(generated.encoded_residual_emitted ?
            (generated.encoded_residual_exact_all_lanes ? "all-lanes" : "unresolved-lanes") : "none")
        + ",\"encoded_prefix_bits\":" + std::to_string(generated.encoded_prefix_bits)
        + ",\"encoded_prefix_nodes\":" + std::to_string(generated.encoded_prefix_nodes)
        + ",\"encoded_prefix_planes\":" + std::to_string(generated.encoded_prefix_planes)
        + ",\"encoded_residual_nodes\":" + std::to_string(generated.encoded_residual_nodes)
        + ",\"encoded_residual_planes\":" + std::to_string(generated.encoded_residual_planes)
        + ",\"encoded_residual_carries\":" + std::to_string(generated.encoded_residual_carries)
        + ",\"encoded_residual_initial_carries\":" + std::to_string(generated.encoded_residual_initial_carries)
        + ",\"encoded_common_nodes\":" + std::to_string(generated.encoded_common_nodes)
        + ",\"encoded_common_planes\":" + std::to_string(generated.encoded_common_planes)
        + ",\"encoded_common_live_values\":" + std::to_string(generated.encoded_common_live_values)
        + ",\"encoded_cold_prefix_nodes\":" + std::to_string(generated.encoded_cold_prefix_nodes)
        + ",\"encoded_cold_prefix_planes\":" + std::to_string(generated.encoded_cold_prefix_planes)
        + ",\"encoded_cold_prefix_live_values\":" + std::to_string(generated.encoded_cold_prefix_live_values)
        + ",\"encoded_proof_attempts\":" + std::to_string(generated.encoded_proof_attempts)
        + ",\"encoded_proof_accepted_rewrites\":" + std::to_string(generated.encoded_proof_accepted_rewrites)
        + ",\"encoded_proof_graph_budget_rejections\":" + std::to_string(generated.encoded_proof_graph_budget_rejections)
        + ",\"encoded_proof_bdd_nodes\":" + std::to_string(generated.encoded_proof_bdd_nodes)
        + ",\"encoded_proof_steps\":" + std::to_string(generated.encoded_proof_steps)
        + ",\"encoded_proof_exhausted\":" + (generated.encoded_proof_exhausted ? "true" : "false")
        + ",\"encoded_proof_reason\":" + quote(generated.encoded_proof_reason)
        + ",\"encoded_reason\":" + quote(generated.encoded_reason)
        + ",\"source_nodes\":" + std::to_string(generated.source_nodes)
        + ",\"predicate_nodes\":" + std::to_string(generated.predicate_nodes)
        + ",\"wordcode_nodes\":" + std::to_string(generated.wordcode_nodes)
        + ",\"header_bytes\":" + std::to_string(generated.header.size());
}

std::string codegen_budget_json_fields(const suffice::CodegenOptions& config) {
    return std::string(",\"encoded_proof_node_budget\":") + std::to_string(config.guarded_residual_options.bdd_node_budget)
        + ",\"encoded_proof_step_budget\":" + std::to_string(config.guarded_residual_options.proof_step_budget)
        + ",\"encoded_proof_attempt_budget\":" + std::to_string(config.guarded_residual_options.rewrite_attempt_budget);
}

void command_emit_cpp(const Options& options) {
    const fs::path destination(options.required("output"));
    ensure_new_output(destination);
    if (options.has("json")) ensure_new_output(fs::path(options.get("json")));
    const auto config = parse_codegen_options(options);
    const auto start = Clock::now();
    const suffice::PackedDataset packed(fs::path(options.required("input")));
    packed.verify_payload();
    const auto validation_ms = elapsed_ms(start);
    const auto compile_start = Clock::now();
    const auto selected = select_program(options, packed.info());
    const auto generated = suffice::emit_cpp(selected.plan, config);
    const auto generation_ms = elapsed_ms(compile_start);
    NewFile output(destination);
    output.write(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(generated.header.data()), generated.header.size()));
    output.commit();
    emit_json("{\"schema_version\":10,\"version\":\"0.13.0\",\"command\":\"emit-cpp\",\"output\":" + quote(path_text(destination))
        + ",\"namespace\":" + quote(config.namespace_name)
        + generated_kernel_json_fields(generated, config)
        + ",\"encoded_strategy\":" + quote(options.get("encoded-strategy", "eager"))
        + ",\"encoded_schedule\":" + quote(options.get("encoded-schedule", "legacy"))
        + ",\"encoded_carry_placement\":" + quote(options.get("encoded-carry-placement", "eager"))
        + ",\"raw_layout\":" + quote(options.get("raw-layout", "interleaved"))
        + codegen_budget_json_fields(config)
        + ",\"payload_validation_ms\":" + number(validation_ms) + ",\"generation_ms\":" + number(generation_ms)
        + ",\"native_compilation_performed\":false,\"total_ms\":" + number(elapsed_ms(start)) + '}', options);
}

void command_emit_portfolio(const Options& options) {
    const fs::path destination(options.required("output"));
    ensure_new_output(destination);
    if (options.has("json")) ensure_new_output(fs::path(options.get("json")));
    const auto config = parse_codegen_options(options, true);
    const auto start = Clock::now();
    const suffice::PackedDataset packed(fs::path(options.required("input")));
    packed.verify_payload();
    const auto validation_ms = elapsed_ms(start);
    const auto compile_start = Clock::now();
    const auto selected = select_program(options, packed.info());
    const auto generated = suffice::emit_cpp_portfolio(selected.plan, config);
    const auto generation_ms = elapsed_ms(compile_start);
    NewFile output(destination);
    output.write(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(generated.header.data()), generated.header.size()));
    output.commit();
    std::ostringstream variants;
    variants << '[';
    for (std::size_t index = 0; index < generated.variants.size(); ++index) {
        if (index) variants << ',';
        const auto& variant = generated.variants[index];
        variants << "{\"index\":" << index << ",\"name\":" << quote(variant.name)
                 << ",\"namespace\":" << quote(variant.namespace_name)
                 << ",\"encoded_strategy\":" << quote(index % 2 ? "guarded-residual" : "residual")
                 << ",\"encoded_schedule\":" << quote(index < 2 ? "pressure" : "bounds-first")
                 << ",\"encoded_carry_placement\":" << quote(index < 2 ? "guarded" : "recompute")
                 << generated_kernel_json_fields(variant.kernel, config) << '}';
    }
    variants << ']';
    emit_json("{\"schema_version\":10,\"version\":\"0.13.0\",\"command\":\"emit-portfolio\",\"output\":" + quote(path_text(destination))
        + ",\"namespace\":" + quote(config.namespace_name) + ",\"description\":" + quote(generated.description)
        + ",\"portfolio_emitted\":true,\"candidate_count\":" + std::to_string(generated.variants.size())
        + ",\"fallback_index\":0,\"variants\":" + variants.str()
        + ",\"encoded_prefix_bits\":" + std::to_string(config.encoded_prefix_bits)
        + ",\"raw_layout\":" + quote(options.get("raw-layout", "interleaved"))
        + codegen_budget_json_fields(config)
        + ",\"runtime_dependency\":\"suffice/portfolio.hpp\""
        + ",\"header_bytes\":" + std::to_string(generated.header.size())
        + ",\"payload_validation_ms\":" + number(validation_ms) + ",\"generation_ms\":" + number(generation_ms)
        + ",\"native_compilation_performed\":false,\"runtime_calibration_performed\":false"
        + ",\"total_ms\":" + number(elapsed_ms(start)) + '}', options);
}

void validate_pair(const suffice::RawDataset& raw, const suffice::PackedDataset& packed) {
    if (raw.info().rows != packed.info().rows || raw.info().channels != packed.info().channels)
        throw std::invalid_argument("raw and packed dataset dimensions differ");
    std::vector<std::int32_t> scratch(packed.info().block_rows);
    for (std::uint64_t block = 0; block < packed.block_count(); ++block) {
        const auto count = packed.block_length(block);
        for (std::uint32_t channel = 0; channel < packed.info().channels; ++channel) {
            packed.decode_block(block, channel, scratch);
            for (std::uint32_t row = 0; row < count; ++row)
                if (scratch[row] != raw.value(block * packed.info().block_rows + row, channel))
                    throw std::invalid_argument("raw and packed payloads differ at row "
                        + std::to_string(block * packed.info().block_rows + row) + ", channel " + std::to_string(channel));
        }
    }
}

struct Timing {
    double median{}, minimum{}, maximum{}, p95{};
};

Timing timing(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto count = values.size();
    Timing result;
    result.minimum = values.front(); result.maximum = values.back();
    result.median = count % 2 ? values[count / 2] : (values[count / 2 - 1] + values[count / 2]) / 2;
    result.p95 = values[std::min(count - 1, static_cast<std::size_t>(std::ceil(double(count) * 0.95)) - 1)];
    return result;
}

std::string timing_json(const Timing& value) {
    return "{\"median_ms\":" + number(value.median) + ",\"min_ms\":" + number(value.minimum)
        + ",\"max_ms\":" + number(value.maximum) + ",\"p95_ms\":" + number(value.p95) + '}';
}

void command_bench(const Options& options) {
    if (options.has("json")) ensure_new_output(fs::path(options.get("json")));
    const auto repeat = options.get_integer<unsigned>("repeat", 5);
    if (!repeat || repeat > 1000) throw std::invalid_argument("--repeat must be in 1..1000");
    const auto validation_start = Clock::now();
    const suffice::PackedDataset packed(fs::path(options.required("packed")));
    const auto channels = options.get_integer<std::uint32_t>("channels", packed.info().channels);
    if (channels != packed.info().channels) throw std::invalid_argument("--channels disagrees with packed metadata");
    const suffice::RawDataset raw(fs::path(options.required("input")), channels);
    packed.verify_payload();
    validate_pair(raw, packed);
    const auto validation_ms = elapsed_ms(validation_start);

    const auto prepare_start = Clock::now();
    const auto selected = select_program(options, packed.info());
    const auto execution = execution_options(options);
    suffice::ProgressiveExecutor progressive(packed, selected.plan, execution);
    suffice::DenseExecutor dense(raw, selected.plan, selected.builtin, true, true);
    suffice::DenseExecutor oracle(raw, selected.plan);
    const auto words = suffice::mask_words(packed.info().rows);
    std::vector<std::uint64_t> reference(words);
    std::array<std::vector<std::uint64_t>, 3> masks;
    for (auto& mask : masks) mask.resize(words);
    std::vector<std::int32_t> full_scratch(static_cast<std::size_t>(3) * channels * packed.info().block_rows);
    const auto preparation_ms = elapsed_ms(prepare_start);
    const auto oracle_stats = oracle.run(reference);
    std::array<suffice::ExecutionStats, 3> stats;
    auto execute = [&](unsigned algorithm) {
        if (algorithm == 0) return dense.run(masks[0]);
        if (algorithm == 1) return suffice::run_packed_full(packed, selected.plan, masks[1], full_scratch);
        return progressive.run(masks[2]);
    };
    auto check = [&](unsigned algorithm) {
        if (masks[algorithm] != reference || stats[algorithm].accepted != oracle_stats.accepted)
            throw std::runtime_error("exact-output verification failed for algorithm " + std::to_string(algorithm));
    };
    const auto warm_start = Clock::now();
    for (unsigned algorithm = 0; algorithm < 3; ++algorithm) { stats[algorithm] = execute(algorithm); check(algorithm); }
    const auto warmup_ms = elapsed_ms(warm_start);
    std::array<std::vector<double>, 3> samples;
    for (auto& sample : samples) sample.reserve(repeat);
    std::uint64_t checksum_accumulator = 0;
    for (unsigned iteration = 0; iteration < repeat; ++iteration) {
        for (unsigned slot = 0; slot < 3; ++slot) {
            const unsigned algorithm = (slot + iteration) % 3;
            const auto start = Clock::now();
            stats[algorithm] = execute(algorithm);
            const auto duration = elapsed_ms(start);
            samples[algorithm].push_back(duration);
            // Exact checking and observable checksum consumption are outside the timer.
            check(algorithm);
            checksum_accumulator ^= suffice::mask_checksum(masks[algorithm]) + 0x9e3779b97f4a7c15ull
                + (checksum_accumulator << 6) + (checksum_accumulator >> 2);
        }
    }
    const std::array<Timing, 3> times{timing(samples[0]), timing(samples[1]), timing(samples[2])};
    std::ostringstream out;
    out << "{\"schema_version\":1,\"command\":\"bench\",\"dataset\":" << info_json(packed.info())
        << ",\"program\":" << quote(selected.specification) << ",\"threshold\":" << selected.plan.threshold()
        << ",\"plan\":" << quote(selected.plan.describe()) << ",\"lowering\":" << quote(progressive.lowering_description()) << ",\"stages\":" << stages_json(execution.stages)
        << ",\"specialization_enabled\":" << (execution.specialize ? "true" : "false")
        << ",\"repeat\":" << repeat << ",\"machine\":" << machine_json()
        << ",\"verification\":{\"exact_match\":true,\"payload_checksum_verified\":true,\"raw_pair_verified\":true,\"checked_every_repetition\":true,\"reference\":\"dense-ir\",\"accepted\":"
        << oracle_stats.accepted << ",\"mask_checksum\":" << suffice::mask_checksum(reference) << '}'
        << ",\"preparation\":{\"validation_ms\":" << number(validation_ms)
        << ",\"compile_and_prepare_ms\":" << number(preparation_ms) << ",\"warmup_ms\":" << number(warmup_ms)
        << ",\"protocol\":\"prevalidated memory-mapped repeated execution; full input validation and correctness checks are outside algorithm timers; rotating algorithm order\"}"
        << ",\"timings\":{\"dense_fused\":" << timing_json(times[0]) << ",\"packed_full\":" << timing_json(times[1])
        << ",\"progressive\":" << timing_json(times[2]) << '}'
        << ",\"dense\":" << stats_json(stats[0]) << ",\"packed_full\":" << stats_json(stats[1])
        << ",\"progressive\":" << stats_json(stats[2])
        << ",\"speedup_vs_dense\":" << ratio(times[0].median, times[2].median)
        << ",\"speedup_vs_packed_full\":" << ratio(times[1].median, times[2].median)
        << ",\"payload_reduction_vs_dense\":" << ratio(double(stats[0].requested_payload_bytes), double(stats[2].requested_payload_bytes))
        << ",\"checksum_accumulator\":" << checksum_accumulator
        << ",\"measurement_notes\":" << quote(measurement_notes)
        << ",\"dense_baseline_note\":\"The dense baseline also receives exact preimage lowering when applicable; dense.kernel identifies its SIMD, scalar, preimage or generic implementation. The separate oracle always evaluates the original IR.\"}";
    emit_json(out.str(), options);
}

bool raw_backend(suffice::AdaptiveBackend backend) {
    return backend == suffice::AdaptiveBackend::Dense || backend == suffice::AdaptiveBackend::RawVector ||
           backend == suffice::AdaptiveBackend::DensePredicate;
}

void command_compare(const Options& options) {
    if (options.has("json")) ensure_new_output(fs::path(options.get("json")));
    const auto total_prepare_start = Clock::now();
    const auto repeat = options.get_integer<unsigned>("repeat", 7);
    if (!repeat || repeat > 1000) throw std::invalid_argument("--repeat must be in 1..1000");
    auto policy = adaptive_options(options);
    const bool packed_only = options.has("packed-only-selection");
    if (packed_only && raw_backend(policy.backend))
        throw std::invalid_argument("a raw backend cannot be forced with --packed-only-selection");
    const auto validation_start = Clock::now();
    const suffice::PackedDataset packed(fs::path(options.required("packed")));
    const auto channels = options.get_integer<std::uint32_t>("channels", packed.info().channels);
    if (channels != packed.info().channels) throw std::invalid_argument("--channels disagrees with packed metadata");
    const suffice::RawDataset raw(fs::path(options.required("input")), channels);
    packed.verify_payload();
    validate_pair(raw, packed);
    const auto validation_ms = elapsed_ms(validation_start);
    policy.inputs_already_verified = true;

    const auto compile_start = Clock::now();
    const auto selected = select_program(options, packed.info());
    const auto program_compile_ms = elapsed_ms(compile_start);
    const auto words = suffice::mask_words(packed.info().rows);
    std::vector<std::uint64_t> reference(words), adaptive_mask(words);
    const auto oracle_start = Clock::now();
    suffice::ExecutionStats oracle_stats;
    {
        suffice::DenseExecutor oracle(raw, selected.plan, {}, false, false, true);
        oracle_stats = oracle.run(reference);
    }
    const auto oracle_ms = elapsed_ms(oracle_start);
    const auto adaptive_start = Clock::now();
    suffice::AdaptiveExecutor adaptive(packed, selected.plan, packed_only ? nullptr : &raw, selected.builtin, policy);
    const auto adaptive_construct_ms = elapsed_ms(adaptive_start);

    struct Fixed {
        suffice::AdaptiveBackend backend;
        std::unique_ptr<suffice::AdaptiveExecutor> executor;
        std::vector<std::uint64_t> mask;
        std::vector<double> samples;
        suffice::ExecutionStats stats;
        Timing heldout;
        double preparation_ms{};
    };
    std::vector<Fixed> fixed;
    fixed.reserve(8);
    auto prepare_fixed = [&](suffice::AdaptiveBackend backend) {
        auto forced = policy;
        forced.backend = backend;
        const auto start = Clock::now();
        Fixed value{backend, std::make_unique<suffice::AdaptiveExecutor>(packed, selected.plan, &raw, selected.builtin, forced),
                    std::vector<std::uint64_t>(words), {}, {}, {}, 0};
        value.samples.reserve(repeat);
        value.preparation_ms = elapsed_ms(start);
        fixed.push_back(std::move(value));
    };
    const auto fixed_prepare_start = Clock::now();
    // A forced dense runner also reports eligibility of all candidates with raw
    // access, without calibrating or constructing those other candidates.
    prepare_fixed(suffice::AdaptiveBackend::Dense);
    const auto fixed_catalogue = fixed.front().executor->report().candidates;
    for (const auto& candidate : fixed_catalogue)
        if (candidate.eligible && candidate.backend != suffice::AdaptiveBackend::Dense) prepare_fixed(candidate.backend);
    const auto fixed_prepare_ms = elapsed_ms(fixed_prepare_start);

    auto check = [&](std::span<const std::uint64_t> mask, const suffice::ExecutionStats& stats, std::string_view label) {
        if (!std::equal(mask.begin(), mask.end(), reference.begin(), reference.end()) ||
            stats.accepted != oracle_stats.accepted || stats.rows != packed.info().rows)
            throw std::runtime_error("held-out exact-output verification failed for " + std::string(label));
    };
    suffice::ExecutionStats adaptive_stats;
    const auto warm_start = Clock::now();
    adaptive_stats = adaptive.run(adaptive_mask); check(adaptive_mask, adaptive_stats, "adaptive");
    for (auto& value : fixed) {
        value.stats = value.executor->run(value.mask);
        check(value.mask, value.stats, suffice::adaptive_backend_name(value.backend));
    }
    const auto heldout_warmup_ms = elapsed_ms(warm_start);
    const auto total_prepare_ms = elapsed_ms(total_prepare_start);
    std::vector<double> adaptive_samples;
    adaptive_samples.reserve(repeat);
    std::uint64_t checksum_accumulator = 0;
    const auto algorithm_count = fixed.size() + 1;
    const std::uint64_t order_seed = policy.sample_seed ^ 0x48454c444f555433ull;
    const auto cycle_rows = suffice::balanced_timing_cycle_rows(algorithm_count);
    const auto heldout_orders = suffice::balanced_timing_orders(algorithm_count, repeat, order_seed);
    const auto heldout_wall_start = Clock::now();
    for (unsigned iteration = 0; iteration < repeat; ++iteration) {
        for (const auto algorithm : heldout_orders[iteration]) {
            if (algorithm == 0) {
                const auto start = Clock::now();
                adaptive_stats = adaptive.run(adaptive_mask);
                adaptive_samples.push_back(elapsed_ms(start));
                check(adaptive_mask, adaptive_stats, "adaptive");
                checksum_accumulator ^= suffice::mask_checksum(adaptive_mask) + 0x9e3779b97f4a7c15ull
                    + (checksum_accumulator << 6) + (checksum_accumulator >> 2);
            } else {
                auto& value = fixed[algorithm - 1];
                const auto start = Clock::now();
                value.stats = value.executor->run(value.mask);
                value.samples.push_back(elapsed_ms(start));
                check(value.mask, value.stats, suffice::adaptive_backend_name(value.backend));
                checksum_accumulator ^= suffice::mask_checksum(value.mask) + 0x9e3779b97f4a7c15ull
                    + (checksum_accumulator << 6) + (checksum_accumulator >> 2);
            }
        }
    }
    const auto heldout_wall_ms = elapsed_ms(heldout_wall_start);
    const auto adaptive_timing = timing(adaptive_samples);
    const Fixed *best = nullptr, *best_eligible = nullptr, *best_raw = nullptr, *best_packed = nullptr, *selected_fixed = nullptr;
    auto choose = [](const Fixed*& current, const Fixed& value) {
        if (!current || value.heldout.median < current->heldout.median) current = &value;
    };
    std::uint64_t prepared_scratch = adaptive.report().selected_scratch_bytes;
    for (auto& value : fixed) {
        value.heldout = timing(value.samples);
        choose(best, value);
        if (raw_backend(value.backend)) choose(best_raw, value); else choose(best_packed, value);
        const auto found = std::find_if(adaptive.report().candidates.begin(), adaptive.report().candidates.end(),
            [&](const auto& c) { return c.backend == value.backend && c.eligible; });
        if (found != adaptive.report().candidates.end()) choose(best_eligible, value);
        if (value.backend == adaptive.report().selected) selected_fixed = &value;
        prepared_scratch += value.executor->report().selected_scratch_bytes;
    }
    if (!best_eligible || !selected_fixed) throw std::logic_error("selected backend absent from held-out comparison");
    auto best_json = [](const Fixed* value) {
        return value ? "{\"backend\":" + quote(suffice::adaptive_backend_name(value->backend)) +
                       ",\"median_ms\":" + number(value->heldout.median) + '}' : std::string("null");
    };
    auto candidate_ratio = [](const Fixed* a, const Fixed* b) {
        return a && b ? ratio(a->heldout.median, b->heldout.median) : std::string("null");
    };
    const auto masks_bytes = std::uint64_t(words) * sizeof(std::uint64_t) * (fixed.size() + 2);
    std::ostringstream out;
    out << "{\"schema_version\":10,\"version\":\"0.13.0\",\"command\":\"compare\",\"dataset\":" << info_json(packed.info())
        << ",\"program\":" << quote(selected.specification) << ",\"threshold\":" << selected.plan.threshold()
        << ",\"plan\":" << quote(selected.plan.describe()) << ",\"repeat\":" << repeat
        << ",\"stages\":" << stages_json(policy.execution.stages) << ",\"tile_rows\":" << policy.tile_rows
        << ",\"progressive_specialization_enabled\":" << (policy.execution.specialize ? "true" : "false")
        << ",\"machine\":" << machine_json()
        << ",\"representation_policy\":{\"adaptive_has_raw\":" << (packed_only ? "false" : "true")
        << ",\"fixed_comparisons_have_raw\":true,\"selection_universe\":" << quote(packed_only ? "packed-only" : "raw-and-packed") << '}'
        << ",\"verification\":{\"exact_match\":true,\"payload_checksum_verified\":true,\"raw_pair_verified\":true,\"checked_every_repetition\":true,\"reference\":\"original-dense-ir\",\"accepted\":"
        << oracle_stats.accepted << ",\"mask_checksum\":" << suffice::mask_checksum(reference)
        << ",\"heldout_mask_checks\":" << std::uint64_t(repeat) * algorithm_count << '}'
        << ",\"preparation\":{\"validation_ms\":" << number(validation_ms)
        << ",\"program_compile_ms\":" << number(program_compile_ms) << ",\"reference_oracle_ms\":" << number(oracle_ms)
        << ",\"adaptive_construct_wall_ms\":" << number(adaptive_construct_ms)
        << ",\"fixed_construct_wall_ms\":" << number(fixed_prepare_ms)
        << ",\"heldout_warmup_ms\":" << number(heldout_warmup_ms)
        << ",\"total_comparison_prepare_ms\":" << number(total_prepare_ms)
        << ",\"protocol\":\"complete input validation, adaptive calibration, forced fixed-backend setup and fresh warmup precede new shuffled timing rounds; no calibration timing sample is reused; same immutable data, not held-out data\"}"
        << ",\"fresh_execution\":{\"protocol\":\"randomized Williams cycles; within-round position and immediate-predecessor balance in complete cycles\""
        << ",\"cycle_rows\":" << cycle_rows << ",\"complete_cycles\":" << repeat / cycle_rows
        << ",\"partial_cycle_rounds\":" << repeat % cycle_rows
        << ",\"order_seed\":" << order_seed << ",\"round_order\":[";
    for (std::size_t round = 0; round < heldout_orders.size(); ++round) {
        if (round) out << ',';
        out << '[';
        for (std::size_t slot = 0; slot < heldout_orders[round].size(); ++slot) {
            if (slot) out << ',';
            const auto index = heldout_orders[round][slot];
            out << quote(index == 0 ? std::string_view("adaptive") : suffice::adaptive_backend_name(fixed[index - 1].backend));
        }
        out << ']';
    }
    out << "]},\"selection\":" << adaptive_report_json(adaptive.report())
        << ",\"adaptive\":{\"backend\":" << quote(suffice::adaptive_backend_name(adaptive.report().selected))
        << ",\"description\":" << quote(adaptive.description()) << ",\"timing\":" << timing_json(adaptive_timing)
        << ",\"samples_ms\":" << samples_json(adaptive_samples) << ",\"stats\":" << stats_json(adaptive_stats) << '}'
        << ",\"fixed\":[";
    for (std::size_t i = 0; i < fixed.size(); ++i) {
        if (i) out << ',';
        const auto& value = fixed[i];
        out << "{\"backend\":" << quote(suffice::adaptive_backend_name(value.backend))
            << ",\"representation\":" << quote(raw_backend(value.backend) ? "raw" : "packed")
            << ",\"description\":" << quote(value.executor->description())
            << ",\"timing\":" << timing_json(value.heldout) << ",\"samples_ms\":" << samples_json(value.samples)
            << ",\"stats\":" << stats_json(value.stats) << ",\"preparation_wall_ms\":" << number(value.preparation_ms)
            << ",\"preparation\":" << adaptive_report_json(value.executor->report()) << '}';
    }
    out << "],\"unavailable_fixed\":[";
    bool first = true;
    for (const auto& c : fixed_catalogue) if (!c.eligible) {
        if (!first) out << ',';
        first = false;
        out << "{\"backend\":" << quote(suffice::adaptive_backend_name(c.backend)) << ",\"reason\":" << quote(c.reason) << '}';
    }
    out << "],\"best\":{\"overall_fixed\":" << best_json(best)
        << ",\"eligible_selection_fixed\":" << best_json(best_eligible)
        << ",\"raw_fixed\":" << best_json(best_raw) << ",\"packed_fixed\":" << best_json(best_packed) << '}'
        << ",\"selection_regret\":{\"adaptive_over_best_eligible_fixed\":" << ratio(adaptive_timing.median, best_eligible->heldout.median)
        << ",\"selected_fixed_over_best_eligible_fixed\":" << ratio(selected_fixed->heldout.median, best_eligible->heldout.median)
        << ",\"adaptive_over_all_fixed\":" << ratio(adaptive_timing.median, best->heldout.median)
        << ",\"note\":\"ratios near 1 are desirable; independent timing noise may put adaptive below 1; eligible selection respects representation restrictions\"}"
        << ",\"comparisons\":{\"best_raw_over_best_packed\":" << candidate_ratio(best_raw, best_packed)
        << ",\"best_raw_over_adaptive\":" << (best_raw ? ratio(best_raw->heldout.median, adaptive_timing.median) : "null")
        << ",\"legacy_dense_over_adaptive\":" << ratio(fixed.front().heldout.median, adaptive_timing.median) << '}'
        << ",\"memory\":{\"prepared_runner_scratch_bytes\":" << prepared_scratch
        << ",\"comparison_masks_bytes\":" << masks_bytes
        << ",\"modeled_retained_execution_workspace_bytes\":" << prepared_scratch + masks_bytes
        << ",\"note\":\"comparison retains one adaptive and all fixed runners; individual preparation peaks are in their reports; figures exclude mappings, process RSS and object/string overhead\"}"
        << ",\"heldout_wall_ms\":" << number(heldout_wall_ms)
        << ",\"checksum_accumulator\":" << checksum_accumulator
        << ",\"measurement_notes\":" << quote(measurement_notes) << '}';
    emit_json(out.str(), options);
}

void help() {
    std::cout << R"HELP(Suffice 0.13.0 - owned snapshots, block proofs and selective payload execution

Usage:
  suffice generate --output data.i32 [--rows N] [--channels C]
                   [--pattern clustered|uniform|boundary|smooth|alternating|constant|heterogeneous]
                   [--seed N] [--json result.json]
  suffice pack --input data.i32 --output data.sfp [--channels C]
               [--block-rows 64] [--verify] [--json result.json]
  suffice inspect --input data.sfp [--verify] [--json result.json]
  suffice run --input data.sfp [--program fir3|delta|identity|energy|file.suf]
              [--threshold N] [--stages 4,8,16,32] [--no-specialize]
              [--raw data.i32] [--backend auto|dense|raw-vector|packed-vector|
               encoded-predicate|dense-predicate|progressive|encoded-wordcode|encoded-frontier]
              [--calibration-repeat 3] [--tile-rows 256] [--max-scratch-mib 128]
              [--first-row N] [--row-count N] [--mask result.mask] [--json result.json]
  suffice emit-cpp --input data.sfp --program builtin|file.suf --output kernel.hpp
                   [--threshold N] [--namespace suffice_generated]
                   [--encoded-strategy eager|frontier|residual|guarded-residual] [--encoded-prefix-bits 12]
                   [--encoded-carry-placement eager|guarded|recompute]
                   [--encoded-proof-nodes 65536] [--encoded-proof-steps 1000000]
                   [--encoded-proof-attempts 256]
                   [--encoded-schedule legacy|pressure|bounds-first] [--raw-layout interleaved|planar]
                   [--encoded-plane-binding per-plane|grouped]
                   [--encoded-repair-policy batch256|word64|hybrid]
                   [--scalar-only] [--original-ir] [--json generation.json]
  suffice emit-portfolio --input data.sfp --program builtin|file.suf --output portfolio.hpp
                         [--threshold N] [--namespace suffice_generated]
                         [--encoded-prefix-bits 12] [--raw-layout interleaved|planar]
                         [--encoded-plane-binding per-plane|grouped]
                         [--encoded-proof-nodes 65536] [--encoded-proof-steps 1000000]
                         [--encoded-proof-attempts 256]
                         [--scalar-only] [--original-ir] [--json generation.json]
  suffice bench --input data.i32 --packed data.sfp [--channels C]
                [--program fir3|delta|identity|energy|file.suf] [--threshold N]
                [--stages 4,8,16,32] [--repeat 5] [--no-specialize]
                [--json result.json]
  suffice compare --input data.i32 --packed data.sfp [--channels C]
                  [--program builtin|file.suf] [--threshold N] [--repeat 7]
                  [--backend auto] [--calibration-repeat 3] [--tile-rows 256]
                  [--max-scratch-mib 128] [--packed-only-selection]
                  [--stages 4,8,16,32] [--no-specialize] [--json result.json]

Builtins use channel zero. Default thresholds: identity 0, delta/fir3 100000,
energy 1000000. A .suf file specifies its own threshold. Comparisons are strict >.
Sample offsets clamp to the first/last row. Raw files contain interleaved signed
int32 little-endian values with no header. Masks contain little-endian uint64
words; row zero is the low bit of the first word, with zero padding at the end.

Every output refuses replacement of an existing path. Pack block sizes must be
powers of two in 32..65536. Stages strictly increase and finish at 32. Bench
verifies packed integrity and raw pairing before timing and exact mask equality
after every repetition. Run verifies packed integrity before executing.

Run defaults to sampled calibration over original global input ranges.
Run/compare calibration options:
  --calibration sampled|full  --sample-rows 32768  --sample-windows 8
  --sample-policy representative|legacy-endpoints  --sample-seed N
  --calibration-work-rows 1048576  --calibration-ms 25
  --max-calibration-mib 128  --frontier-prefix-bits 12  --no-race
  --frontier-repair sparse|hybrid|tile  --frontier-dense-percent 25
Sampled preparation bounds requested output-row evaluations (reference, warmup,
and timed calls), including separate untimed boundary probes. Integrity scans
and decoder block reads are separate. Representative performance windows use
aligned random strata; an incomplete final alignment unit is reported outside
the timing population. These samples are not a latency guarantee for unseen rows.
The time budget is soft, checked after complete rounds; zero disables it.
Full/forced modes explicitly do complete reference work without the sampled-row
budget. No-race measures all candidates in every completed round.
 Supplying --raw makes
raw backends available after exact raw/packed pairing. Forced backends perform
correctness warmup but no timed calibration. All preparation costs are reported.
Compare performs fresh timing rounds after calibration; none of the calibration
samples are reused. --packed-only-selection withholds raw input from selection,
while fixed raw comparisons are still measured and explicitly labeled.
--no-specialize affects the progressive candidate only. Bench retains its legacy
three-path comparison schema. A scratch cap limits each candidate's modeled
workspace. The calibration cap bounds the conservative sum of modeled reference,
mask and candidate workspaces, not process RSS or compiler/object metadata.
Emit-cpp produces a reusable C++20 header; it does not invoke a native compiler.
Generated PreparedKernel validates native int32 inputs once and has scalar/AVX2
run/run_range APIs. See docs/codegen.md for integration and benchmark details.
Emit-portfolio emits four fixed residual implementations and a reusable
PreparedEncodedPortfolio. It depends on suffice/portfolio.hpp and starts with
the all-lane pressure/guarded fallback. Call calibrate with an independently
computed expected mask, a separate scratch mask, explicit row/time budgets and
expected remaining calls. Calibration uses complete balanced cycles, verifies
every call, and reports costs and the observed break-even point. Subsequent
run/run_range calls use the selected implementation without recalibration.
See docs/portfolio_v0_7.md. Header generation performs no native compilation
or runtime calibration. Source liveness counters are not physical memory bounds.

Generated patterns are deterministic: uniform uses all int32 bit patterns;
clustered has 256-row signed plateaus with small noise and occasional near-zero
regions; boundary is mostly -16..16 with sparse +/-100001 samples; smooth is an
8192-row triangle; alternating switches between approximately +/-1e9; constant
is 12345 plus channel index. These are synthetic workloads, not AI model claims.

Requested payload bytes are software counters, not measured DRAM/PCIe traffic.
This version executes on CPU. Use --help or --version for this information.
)HELP";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc < 2) { help(); return 0; }
        const std::string_view command(argv[1]);
        if (command == "--version" || command == "version") { std::cout << "suffice 0.13.0\n"; return 0; }
        for (int index = 1; index < argc; ++index)
            if (std::string_view(argv[index]) == "--help" || std::string_view(argv[index]) == "-h") { help(); return 0; }
        if (command == "generate") command_generate(Options(argc, argv, {"output", "rows", "channels", "pattern", "seed", "json"}));
        else if (command == "pack") command_pack(Options(argc, argv, {"input", "output", "channels", "block-rows", "json"}, {"verify"}));
        else if (command == "inspect") command_inspect(Options(argc, argv, {"input", "json"}, {"verify"}));
        else if (command == "run") command_run(Options(argc, argv, {"input", "raw", "program", "threshold", "stages", "mask", "json", "backend", "calibration-repeat", "tile-rows", "max-scratch-mib", "first-row", "row-count", "calibration", "sample-rows", "sample-windows", "sample-policy", "sample-seed", "calibration-work-rows", "calibration-ms", "max-calibration-mib", "frontier-prefix-bits", "frontier-repair", "frontier-dense-percent"}, {"no-specialize", "no-race"}));
        else if (command == "emit-cpp") command_emit_cpp(Options(argc, argv, {"input", "output", "program", "threshold", "namespace", "json", "encoded-strategy", "encoded-prefix-bits", "encoded-schedule", "encoded-carry-placement", "encoded-plane-binding", "encoded-repair-policy", "encoded-proof-nodes", "encoded-proof-steps", "encoded-proof-attempts", "raw-layout"}, {"scalar-only", "original-ir"}));
        else if (command == "emit-portfolio") command_emit_portfolio(Options(argc, argv, {"input", "output", "program", "threshold", "namespace", "json", "encoded-prefix-bits", "encoded-plane-binding", "encoded-proof-nodes", "encoded-proof-steps", "encoded-proof-attempts", "raw-layout"}, {"scalar-only", "original-ir"}));
        else if (command == "bench") command_bench(Options(argc, argv, {"input", "packed", "channels", "program", "threshold", "stages", "repeat", "json"}, {"no-specialize"}));
        else if (command == "compare") command_compare(Options(argc, argv, {"input", "packed", "channels", "program", "threshold", "stages", "repeat", "json", "backend", "calibration-repeat", "tile-rows", "max-scratch-mib", "calibration", "sample-rows", "sample-windows", "sample-policy", "sample-seed", "calibration-work-rows", "calibration-ms", "max-calibration-mib", "frontier-prefix-bits", "frontier-repair", "frontier-dense-percent"}, {"no-specialize", "packed-only-selection", "no-race"}));
        else throw std::invalid_argument("unknown command: " + std::string(command));
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "suffice: " << error.what() << '\n';
        return 2;
    }
}
