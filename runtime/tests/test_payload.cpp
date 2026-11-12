#include "suffice/payload.hpp"

#include <zstd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Bytes = std::vector<std::uint8_t>;
using suffice::Bounds;
using suffice::Decision;
using suffice::Program;
using namespace suffice::payload;

struct Counts {
    std::uint64_t groups{}, pipeline_runs{}, cache_replays{}, rejections{};
    std::uint64_t compared_rows{}, compared_bytes{}, feature_bound_checks{};
    std::uint64_t certificate_row_checks{};
} counts;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("payload test: " + message);
}
void require(bool condition, const std::string& message) {
    if (!condition) fail(message);
}
template<class F>
void rejects(F&& function, const std::string& message,
             const std::string& expected_message = {}) {
    try {
        function();
    } catch (const std::bad_alloc&) {
        fail(message + ": rejection must not depend on allocation failure");
    } catch (const std::exception& error) {
        if (!expected_message.empty() &&
            std::string(error.what()).find(expected_message) == std::string::npos)
            fail(message + ": unexpected error: " + error.what());
        ++counts.rejections;
        return;
    }
    fail(message + ": expected rejection");
}
template<class F> void group(const char* name, F&& function) {
    function();
    ++counts.groups;
    std::cout << "PASS payload " << name << '\n';
}

class TempDirectory {
public:
    TempDirectory() {
        static std::atomic<unsigned> sequence{0};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                ("suffice-payload-test-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(path)) return;
        }
        fail("could not create fixture directory");
    }
    ~TempDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};
void put32(std::uint8_t* out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void put64(std::uint8_t* out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint32_t get32(const std::uint8_t* in) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= std::uint32_t(in[i]) << (8 * i);
    return value;
}
std::uint64_t fixture_hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 14695981039346656037ULL;
    for (const auto byte : bytes) value = (value ^ byte) * 1099511628211ULL;
    return value;
}
void write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty())
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    if (!file) fail("fixture write failed");
}
Bytes read_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto length = file.tellg();
    if (length < 0) fail("fixture read length failed");
    Bytes bytes(static_cast<std::size_t>(length));
    file.seekg(0);
    if (!bytes.empty())
        file.read(reinterpret_cast<char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    if (!file) fail("fixture read failed");
    return bytes;
}
void check_zstd(std::size_t result) {
    if (ZSTD_isError(result)) fail(std::string("fixture compression: ") + ZSTD_getErrorName(result));
}
Bytes compress_frame(std::span<const std::uint8_t> plain, bool include_content_size = true) {
    const std::unique_ptr<ZSTD_CCtx, decltype(&ZSTD_freeCCtx)>
        context(ZSTD_createCCtx(), &ZSTD_freeCCtx);
    if (!context) throw std::bad_alloc();
    check_zstd(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_compressionLevel, 1));
    check_zstd(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_checksumFlag, 1));
    check_zstd(ZSTD_CCtx_setParameter(context.get(), ZSTD_c_contentSizeFlag,
                                    include_content_size ? 1 : 0));
    const auto capacity = ZSTD_compressBound(plain.size());
    check_zstd(capacity);
    Bytes compressed(capacity);
    static const std::uint8_t empty = 0;
    const auto length = ZSTD_compress2(context.get(), compressed.data(), compressed.size(),
                                      plain.empty() ? &empty : plain.data(), plain.size());
    check_zstd(length);
    compressed.resize(length);
    return compressed;
}
void refresh_store_metadata(Bytes& bytes, std::size_t rows) {
    std::fill(bytes.begin() + 56, bytes.begin() + 64, std::uint8_t{0});
    put64(bytes.data() + 56,
          fixture_hash(std::span<const std::uint8_t>(bytes).first(64 + rows * 24)));
}
Bytes make_store(std::span<const std::uint8_t> raw, std::uint32_t channels,
                 const std::vector<Bytes>& plaintext, const std::vector<Bytes>& frames) {
    require(plaintext.size() == frames.size(), "fixture frame count");
    const auto rows = plaintext.size();
    const auto index_bytes = rows * 24;
    Bytes store(64 + index_bytes);
    const std::array<std::uint8_t, 8> magic{'S','U','F','P','A','Y','0','1'};
    std::copy(magic.begin(), magic.end(), store.begin());
    put32(store.data() + 8, 1);
    put32(store.data() + 12, 1);
    put64(store.data() + 16, rows);
    put32(store.data() + 24, channels);
    put32(store.data() + 28, 24);
    put64(store.data() + 32, fixture_hash(raw));
    put64(store.data() + 40, index_bytes);
    for (std::size_t row = 0; row < rows; ++row) {
        auto* entry = store.data() + 64 + row * 24;
        put64(entry, store.size());
        put32(entry + 8, static_cast<std::uint32_t>(frames[row].size()));
        put32(entry + 12, static_cast<std::uint32_t>(plaintext[row].size()));
        put64(entry + 16, fixture_hash(plaintext[row]));
        store.insert(store.end(), frames[row].begin(), frames[row].end());
    }
    put64(store.data() + 48, store.size());
    refresh_store_metadata(store, rows);
    return store;
}

struct Fixture {
    static constexpr std::uint32_t channels = 2, block_rows = 64;
    TempDirectory directory;
    std::uint64_t rows;
    std::vector<std::int32_t> features;
    std::vector<Bytes> plaintext, frames;
    Bytes raw, packed, store;

    explicit Fixture(std::uint64_t count = 323) : rows(count) {
        features.resize(static_cast<std::size_t>(rows) * channels);
        plaintext.resize(static_cast<std::size_t>(rows));
        for (std::uint64_t row = 0; row < rows; ++row) {
            const auto base = static_cast<std::size_t>(row) * channels;
            // Physical blocks deliberately alternate true, false, and mixed.
            if ((row / block_rows) % 3 == 0) {
                features[base] = 9 + static_cast<std::int32_t>(row % 3);
                features[base + 1] = 6 + static_cast<std::int32_t>(row % 5);
            } else if ((row / block_rows) % 3 == 1) {
                features[base] = -9 + static_cast<std::int32_t>(row % 3);
                features[base + 1] = -4 + static_cast<std::int32_t>(row % 3);
            } else {
                features[base] = static_cast<std::int32_t>((row * 17 + 3) % 33) - 16;
                features[base + 1] = static_cast<std::int32_t>((row * 11 + 5) % 29) - 14;
            }
            auto& bytes = plaintext[static_cast<std::size_t>(row)];
            if (row % 19) {
                bytes.resize(static_cast<std::size_t>(1 + (row * 31) % 97));
                for (std::size_t i = 0; i < bytes.size(); ++i)
                    bytes[i] = static_cast<std::uint8_t>((row * 43 + i * 17) & 255);
            }
        }
        const auto feature = [&](std::size_t row, std::int32_t a, std::int32_t b) {
            if (row < rows) { features[row * channels] = a; features[row * channels + 1] = b; }
        };
        feature(0, 9, 8); feature(1, 5, 9); feature(2, 8, 7);
        feature(128, 8, 10); feature(129, -8, -9);
        feature(320, 8, 9); feature(321, -7, -2); feature(322, 11, 10);
        if (rows > 1) plaintext[1] = {0, 1, 2, 255};
        if (rows > 2) plaintext[2] = {0, 2, 1, 255}; // Same length and sum, different bytes.
        if (rows > 3) {
            plaintext[3].resize(256);
            for (unsigned i = 0; i < 256; ++i) plaintext[3][i] = static_cast<std::uint8_t>(i);
        }
        raw.resize(features.size() * 4);
        for (std::size_t i = 0; i < features.size(); ++i)
            put32(raw.data() + i * 4, std::bit_cast<std::uint32_t>(features[i]));
        write_file(directory.path / "features.i32", raw);
        (void)suffice::pack_file(directory.path / "features.i32",
                                directory.path / "features.sfc", channels, block_rows);
        packed = read_file(directory.path / "features.sfc");
        for (const auto& bytes : plaintext) frames.push_back(compress_frame(bytes));
        store = make_store(raw, channels, plaintext, frames);
        write_file(directory.path / "payload.sfp", store);
    }
    std::shared_ptr<const Snapshot> snapshot(Validation validation = Validation::Full,
                                              Limits limits = {}) const {
        return Snapshot::copy_bytes(raw, channels, packed, store, limits, validation);
    }
    bool selected(std::uint64_t row) const {
        const auto base = static_cast<std::size_t>(row) * channels;
        return std::min(features[base], features[base + 1]) > 3;
    }
    std::uint64_t decoded_bytes() const {
        std::uint64_t total = 0;
        for (const auto& bytes : plaintext) total += bytes.size();
        return total;
    }
    std::uint32_t max_row_bytes() const {
        std::size_t maximum = 0;
        for (const auto& bytes : plaintext) maximum = std::max(maximum, bytes.size());
        return static_cast<std::uint32_t>(maximum);
    }
};

Program selection_program() {
    Program program;
    const auto a = program.load(0), b = program.load(1);
    program.output(program.min(a, b), 3);
    return program;
}
Program constant_program(bool value) {
    Program program;
    program.output(program.constant(value ? 1 : 0), 0);
    return program;
}
struct Row {
    std::uint64_t id;
    Bytes bytes;
    friend bool operator==(const Row&, const Row&) = default;
};
using Rows = std::vector<Row>;
using Select = std::function<bool(std::uint64_t)>;
struct Collect final : RowSink {
    Rows rows;
    void row(std::uint64_t id, std::span<const std::uint8_t> bytes) override {
        rows.push_back({id, Bytes(bytes.begin(), bytes.end())});
    }
};
Rows oracle(const Fixture& fixture, std::uint64_t first, std::uint64_t count,
            const Select& select) {
    Rows result;
    for (std::uint64_t row = first; row < first + count; ++row)
        if (select(row)) result.push_back({row, fixture.plaintext[static_cast<std::size_t>(row)]});
    return result;
}
std::uint64_t total_bytes(const Rows& rows) {
    std::uint64_t total = 0;
    for (const auto& row : rows) total += row.bytes.size();
    return total;
}
void compare_rows(const Rows& actual, const Rows& expected, const std::string& context) {
    require(actual == expected, context + ": ordered identifiers/full decoded bytes differ");
    counts.compared_rows += actual.size();
    counts.compared_bytes += total_bytes(actual);
}
RunStats check_run(Worker& worker, Backend backend, const Fixture& fixture,
                   std::uint64_t first, std::uint64_t count, const Select& select,
                   OutputLimits limits = {}) {
    Collect sink;
    const auto expected = oracle(fixture, first, count, select);
    const auto stats = worker.run(backend, first, count, sink, limits);
    const auto context = std::string(backend_name(backend)) + " [" +
        std::to_string(first) + "," + std::to_string(count) + "]";
    compare_rows(sink.rows, expected, context);
    require(stats.first_row == first && stats.row_count == count, context + ": extent statistics");
    require(stats.backend == (backend == Backend::Auto ? Backend::Zoned : backend),
            context + ": effective backend");
    require(stats.selected_rows == expected.size(), context + ": selected count");
    require(stats.decoded_payload_bytes == total_bytes(expected) &&
            stats.output_bytes == total_bytes(expected), context + ": decoded/delivered counters");
    std::uint64_t compressed = 0;
    for (const auto& row : expected) compressed += fixture.frames[static_cast<std::size_t>(row.id)].size();
    require(stats.compressed_payload_bytes == compressed, context + ": compressed-byte counter");
    if (stats.backend == Backend::Zoned)
        require(stats.zone_true_rows + stats.zone_false_rows + stats.zone_refined_rows == count,
                context + ": zone partition");
    std::vector<std::uint64_t> expected_mask(static_cast<std::size_t>((count + 63) / 64));
    for (std::uint64_t i = 0; i < count; ++i)
        if (select(first + i)) expected_mask[static_cast<std::size_t>(i / 64)] |= UINT64_C(1) << (i % 64);
    const auto mask = worker.last_mask();
    require(mask.size() == expected_mask.size() &&
            std::equal(mask.begin(), mask.end(), expected_mask.begin()), context + ": exact mask/tail");
    ++counts.pipeline_runs;
    return stats;
}
void check_certificates(const Query& query, const Fixture& fixture, const Select& select) {
    const auto decisions = query.block_decisions();
    require(decisions.size() == (fixture.rows + Fixture::block_rows - 1) / Fixture::block_rows,
            "zone certificate count");
    for (std::uint64_t row = 0; row < fixture.rows; ++row) {
        const auto decision = decisions[static_cast<std::size_t>(row / Fixture::block_rows)];
        if (decision != Decision::Unknown)
            require((decision == Decision::True) == select(row), "unsound physical-block certificate");
        ++counts.certificate_row_checks;
    }
}
const std::array<Backend, 5> backends{
    Backend::Scalar, Backend::Dense, Backend::Wordcode, Backend::Zoned, Backend::Auto
};
}

void test_payload() {
    counts = {};
    Fixture fixture;
    const auto snapshot = fixture.snapshot();
    const auto query = Query::prepare(snapshot, selection_program());
    const Select selected = [&](std::uint64_t row) { return fixture.selected(row); };
    const Select all = [](std::uint64_t) { return true; };
    const Select none = [](std::uint64_t) { return false; };
    require(query->wordcode().complete && query->predicate().complete, "main fixture lowers");

    group("complete binary outputs, all backends, slices and tails", [&] {
        require(snapshot->stats().all_payloads_verified, "default admission verifies every payload");
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 18> ranges{{
            {0,323}, {0,0}, {323,0}, {0,1}, {1,1}, {1,63}, {7,64}, {8,65},
            {63,64}, {64,65}, {65,127}, {255,1}, {255,64}, {256,65},
            {257,66}, {322,1}, {1,322}, {128,129}
        }};
        for (const auto backend : backends) {
            Worker worker(query);
            for (const auto& [first, count] : ranges)
                (void)check_run(worker, backend, fixture, first, count, selected);
        }
        require(fixture.plaintext[1] != fixture.plaintext[2] &&
                fixture.plaintext[1].size() == fixture.plaintext[2].size(),
                "byte oracle includes equal-length differing payloads");
        Worker worker(query);
        Collect left, right;
        worker.run(Backend::Auto, 0, 129, left);
        worker.run(Backend::Auto, 129, fixture.rows - 129, right);
        left.rows.insert(left.rows.end(), right.rows.begin(), right.rows.end());
        compare_rows(left.rows, oracle(fixture, 0, fixture.rows, selected), "slice composition");
        counts.pipeline_runs += 2;
    });

    group("independent physical bounds and true/false/unknown zone certificates", [&] {
        const std::array<Decision, 6> expected{
            Decision::True, Decision::False, Decision::Unknown,
            Decision::True, Decision::False, Decision::Unknown
        };
        require(std::equal(query->block_decisions().begin(), query->block_decisions().end(),
                           expected.begin(), expected.end()), "fixture covers all zone outcomes");
        check_certificates(*query, fixture, selected);
        const auto check_bounds = [&](std::uint32_t channel, std::uint64_t first, std::uint64_t count) {
            const auto begin = (first / Fixture::block_rows) * Fixture::block_rows;
            const auto end = std::min(fixture.rows,
                ((first + count - 1) / Fixture::block_rows + 1) * Fixture::block_rows);
            Bounds expected_bounds{std::numeric_limits<std::int32_t>::max(),
                                   std::numeric_limits<std::int32_t>::min()};
            for (auto row = begin; row < end; ++row) {
                const auto value = fixture.features[static_cast<std::size_t>(row) * Fixture::channels + channel];
                expected_bounds.lo = std::min(expected_bounds.lo, suffice::i64(value));
                expected_bounds.hi = std::max(expected_bounds.hi, suffice::i64(value));
            }
            require(snapshot->feature_bounds(channel, first, count) == expected_bounds,
                    "whole-block bounds differ from independent extrema");
            ++counts.feature_bound_checks;
        };
        for (std::uint32_t channel = 0; channel < Fixture::channels; ++channel) {
            for (std::uint64_t row = 0; row < fixture.rows; ++row) check_bounds(channel, row, 1);
            check_bounds(channel, 7, 260);
            check_bounds(channel, 63, 2);
            check_bounds(channel, 1, 322);
        }
        rejects([&] { (void)snapshot->feature_bounds(2, 0, 1); }, "feature-bound channel");
        rejects([&] { (void)snapshot->feature_bounds(0, 0, 0); }, "empty feature-bound query");
        rejects([&] { (void)snapshot->feature_bounds(0, 322, UINT64_MAX); }, "feature-bound overflow");
    });

    group("copied buffer and source-program independence", [&] {
        auto raw = fixture.raw, packed = fixture.packed, store = fixture.store;
        auto owned = Snapshot::copy_bytes(raw, Fixture::channels, packed, store);
        auto source = selection_program();
        auto prepared = Query::prepare(owned, source);
        source.output(source.constant(0), 100); // A later builder edit cannot relabel the query.
        std::fill(raw.begin(), raw.end(), 0);
        std::fill(packed.begin(), packed.end(), 0);
        std::fill(store.begin(), store.end(), 0);
        raw.clear(); raw.shrink_to_fit();
        packed.clear(); packed.shrink_to_fit();
        store.clear(); store.shrink_to_fit();
        for (const auto backend : backends) {
            Worker worker(prepared);
            (void)check_run(worker, backend, fixture, 7, 260, selected);
        }
    });

    group("file-loaded snapshot survives source mutation and deletion", [&] {
        TempDirectory directory;
        const auto raw = directory.path / "raw.i32";
        const auto packed = directory.path / "packed.sfc";
        const auto payload = directory.path / "payload.sfp";
        write_file(raw, fixture.raw); write_file(packed, fixture.packed); write_file(payload, fixture.store);
        auto loaded = Snapshot::load(raw, Fixture::channels, packed, payload);
        write_file(raw, Bytes{1}); write_file(packed, Bytes{2}); write_file(payload, Bytes{3});
        require(std::filesystem::remove(raw) && std::filesystem::remove(packed) &&
                std::filesystem::remove(payload), "remove admitted source files");
        Worker worker(Query::prepare(loaded, selection_program()));
        (void)check_run(worker, Backend::Auto, fixture, 0, fixture.rows, selected);
        (void)check_run(worker, Backend::Scalar, fixture, 63, 65, selected);
    });

    group("worker retains stable query and snapshot owners", [&] {
        std::weak_ptr<const Snapshot> weak_snapshot;
        std::weak_ptr<const Query> weak_query;
        {
            auto owned = fixture.snapshot();
            auto prepared = Query::prepare(owned, selection_program());
            weak_snapshot = owned; weak_query = prepared;
            auto worker = std::make_unique<Worker>(prepared);
            owned.reset(); prepared.reset();
            require(!weak_snapshot.expired() && !weak_query.expired(), "worker retains both owners");
            (void)check_run(*worker, Backend::Wordcode, fixture, 1, 322, selected);
            (void)check_run(*worker, Backend::Auto, fixture, 255, 68, selected);
            worker.reset();
        }
        require(weak_snapshot.expired() && weak_query.expired(), "worker releases retained owners");
    });

    group("complete-result cache and exact query/range identity", [&] {
        const auto expected = oracle(fixture, 1, 258, selected);
        auto cache = ResultCache::build(query, Backend::Auto, 1, 258, 1U << 20);
        Collect sink;
        cache->replay(query, 1, 258, sink);
        compare_rows(sink.rows, expected, "complete-result replay");
        ++counts.cache_replays;
        require(cache->selected_rows() == expected.size() &&
                cache->storage_bytes() >= total_bytes(expected), "cache retained-output accounting");
        const auto reject_replay = [&](const std::shared_ptr<const Query>& other,
                                       std::uint64_t first, std::uint64_t count) {
            Collect output;
            rejects([&] { cache->replay(other, first, count, output); }, "cache identity");
            require(output.rows.empty(), "cache mismatch rejects before callback");
        };
        reject_replay(Query::prepare(snapshot, selection_program()), 1, 258); // Identical semantics, new owner.
        reject_replay(Query::prepare(snapshot, constant_program(true)), 1, 258);
        reject_replay(Query::prepare(fixture.snapshot(), selection_program()), 1, 258);
        reject_replay(query, 2, 258);
        reject_replay(query, 1, 257);
        reject_replay({}, 1, 258);
        rejects([&] { (void)ResultCache::build(query, Backend::Auto, 1, 258, 0); },
                "nonempty cache exceeds zero budget", "budget");
        auto empty_cache = ResultCache::build(query, Backend::Auto, fixture.rows, 0, 0);
        Collect empty;
        empty_cache->replay(query, fixture.rows, 0, empty);
        require(empty.rows.empty() && empty_cache->selected_rows() == 0, "empty cache");
        ++counts.cache_replays;
    });

    group("fused capture preserves complete outputs, tails, budgets and owner identity", [&] {
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 9> ranges{{
            {0,323}, {0,0}, {323,0}, {1,63}, {7,64}, {8,65}, {63,64}, {257,66}, {322,1}
        }};
        for(const auto backend:backends) {
            Worker worker(query);
            for(const auto& [first,count]:ranges) {
                const auto expected=oracle(fixture,first,count,selected);
                const auto bytes=total_bytes(expected);
                const auto capacity=bytes+24*expected.size();
                Collect delivered,replayed;
                auto captured=worker.run_captured(backend,first,count,delivered,capacity,
                                                  {expected.size(),bytes});
                require(captured.cache && !captured.oversized && !captured.failed,
                        "exact capture budget admits candidate");
                compare_rows(delivered.rows,expected,"fused delivery");
                require(captured.pipeline.selected_rows==expected.size() &&
                        captured.pipeline.decoded_payload_bytes==bytes &&
                        captured.pipeline.output_bytes==bytes,"fused pipeline counts");
                require(captured.cache->storage_bytes()<=capacity,"capture capacity cap");
                captured.cache->replay(query,first,count,replayed);
                compare_rows(replayed.rows,expected,"fused candidate replay");
                ++counts.pipeline_runs;++counts.cache_replays;
                if(capacity) {
                    Collect uncached;
                    auto rejected=worker.run_captured(backend,first,count,uncached,capacity-1);
                    require(!rejected.cache && rejected.oversized && !rejected.failed,
                            "undersized optional candidate rejected");
                    compare_rows(uncached.rows,expected,"oversize capture still delivers exact output");
                    ++counts.pipeline_runs;
                }
            }
        }
        Worker worker(query);
        Collect output;
        auto capture=worker.run_captured(Backend::Auto,1,258,output,1U<<20);
        Collect mismatch;
        rejects([&]{capture.cache->replay(Query::prepare(snapshot,selection_program()),1,258,mismatch);},
                "fused owner identity");
        rejects([&]{capture.cache->replay(query,2,258,mismatch);},"fused range identity");
        require(mismatch.rows.empty(),"fused identity rejection before callbacks");
    });

    group("fused capture preflights output and does not return partial candidates", [&] {
        Worker worker(query);
        const auto expected=oracle(fixture,0,fixture.rows,selected);
        Collect limited;
        rejects([&]{(void)worker.run_captured(Backend::Auto,0,fixture.rows,limited,1U<<20,
                                            {expected.size()-1,total_bytes(expected)});},
                "capture output rows preflight","output budget");
        rejects([&]{(void)worker.run_captured(Backend::Auto,0,fixture.rows,limited,1U<<20,
                                            {expected.size(),total_bytes(expected)-1});},
                "capture output bytes preflight","output budget");
        require(limited.rows.empty(),"capture output cap emits nothing");
        struct Stop final:RowSink {
            std::size_t calls{};
            void row(std::uint64_t,std::span<const std::uint8_t>) override {
                if(++calls==3)throw std::runtime_error("capture sink stop");
            }
        } stopped;
        std::unique_ptr<ResultCache> published;
        rejects([&]{auto value=worker.run_captured(Backend::Auto,0,fixture.rows,stopped,1U<<20);
                    published=std::move(value.cache);},"capture sink propagates","capture sink stop");
        require(!published && stopped.calls==3,"failed sink cannot publish partial candidate");
        Collect recovered;
        auto after=worker.run_captured(Backend::Auto,0,fixture.rows,recovered,1U<<20);
        require(after.cache!=nullptr,"worker recovers after failed capture sink");
        compare_rows(recovered.rows,expected,"capture recovery");
        ++counts.pipeline_runs;
    });

    group("signed offset halos use whole-snapshot boundaries", [&] {
        const std::array<std::pair<std::uint64_t, std::uint64_t>, 9> ranges{{
            {0,323}, {1,63}, {63,3}, {64,65}, {127,3}, {191,65}, {257,66}, {322,1}, {323,0}
        }};
        for (const int offset : {-16, -1, 1, 16}) {
            Program source;
            source.output(source.load(0, offset), 3);
            const auto prepared = Query::prepare(snapshot, source);
            require(!prepared->wordcode().complete, "offset load must not use unsupported wordcode");
            const Select select = [&](std::uint64_t row) {
                const auto signed_row = std::int64_t(row) + offset;
                const auto source_row = std::clamp<std::int64_t>(
                    signed_row, 0, static_cast<std::int64_t>(fixture.rows) - 1);
                return fixture.features[static_cast<std::size_t>(source_row) * Fixture::channels] > 3;
            };
            check_certificates(*prepared, fixture, select);
            for (const auto backend : {Backend::Scalar, Backend::Zoned, Backend::Auto}) {
                Worker worker(prepared);
                for (const auto& [first, count] : ranges)
                    (void)check_run(worker, backend, fixture, first, count, select);
            }
            Worker worker(prepared);
            Collect output;
            rejects([&] { worker.run(Backend::Wordcode, 63, 3, output); },
                    "forced unsupported wordcode", "incomplete");
            require(output.rows.empty(), "unsupported backend emits nothing");
        }
    });

    group("one-frame grammar, index extents and metadata admission", [&] {
        const auto reject_store = [&](Bytes bytes, const std::string& message,
                                      const std::string& expected = {}) {
            rejects([&] { (void)Snapshot::copy_bytes(fixture.raw, Fixture::channels,
                            fixture.packed, bytes); }, message, expected);
        };
        auto changed = fixture.store;
        changed[0] ^= 1; reject_store(changed, "store magic");
        changed = fixture.store; put32(changed.data() + 8, 2);
        reject_store(changed, "store version");
        changed = fixture.store; put64(changed.data() + 16, UINT64_MAX);
        reject_store(changed, "store row count");
        changed = fixture.store; put64(changed.data() + 40, UINT64_MAX);
        reject_store(changed, "index length overflow");
        changed = fixture.store; put64(changed.data() + 48, UINT64_MAX);
        reject_store(changed, "declared file extent");
        changed = fixture.store; changed[56] ^= 1;
        reject_store(changed, "metadata checksum");
        changed = fixture.store; put64(changed.data() + 64, UINT64_MAX);
        refresh_store_metadata(changed, fixture.rows);
        reject_store(changed, "absolute offset overflow", "range");
        changed = fixture.store; put32(changed.data() + 64 + 8, UINT32_MAX);
        refresh_store_metadata(changed, fixture.rows);
        reject_store(changed, "compressed range overflow", "range");
        changed = fixture.store; put32(changed.data() + 64 + 12, UINT32_MAX);
        refresh_store_metadata(changed, fixture.rows);
        reject_store(changed, "decoded size above budget", "budget");
        changed = fixture.store; put32(changed.data() + 64 + 24 + 12, 5);
        refresh_store_metadata(changed, fixture.rows);
        reject_store(changed, "directory/frame size disagreement", "size");
        changed = fixture.store; changed.push_back(0);
        put64(changed.data() + 48, changed.size());
        refresh_store_metadata(changed, fixture.rows);
        reject_store(changed, "trailing unindexed bytes", "trailing");
        auto frames = fixture.frames;
        frames[1].insert(frames[1].end(), fixture.frames[0].begin(), fixture.frames[0].end());
        reject_store(make_store(fixture.raw, Fixture::channels, fixture.plaintext, frames),
                     "concatenated frames", "exactly one frame");
        frames = fixture.frames; frames[1].push_back(0);
        reject_store(make_store(fixture.raw, Fixture::channels, fixture.plaintext, frames),
                     "frame plus junk", "exactly one frame");
        frames = fixture.frames; frames[1].pop_back();
        reject_store(make_store(fixture.raw, Fixture::channels, fixture.plaintext, frames),
                     "truncated frame", "exactly one frame");
        frames = fixture.frames; frames[1] = Bytes(10);
        put32(frames[1].data(), 0x184D2A50U); put32(frames[1].data() + 4, 2);
        reject_store(make_store(fixture.raw, Fixture::channels, fixture.plaintext, frames),
                     "skippable frame", "ordinary");
        frames = fixture.frames; frames[1] = compress_frame(fixture.plaintext[1], false);
        reject_store(make_store(fixture.raw, Fixture::channels, fixture.plaintext, frames),
                     "unknown frame content size", "size");
    });

    group("full versus on-demand body validation and failure recovery", [&] {
        auto frames = fixture.frames;
        frames[1].back() ^= 1; // Native content checksum only: frame grammar remains valid.
        auto damaged = make_store(fixture.raw, Fixture::channels, fixture.plaintext, frames);
        rejects([&] { (void)Snapshot::copy_bytes(fixture.raw, Fixture::channels, fixture.packed, damaged); },
                "full admission rejects an unselected corrupt row", "decompression failed");
        const auto deferred = Snapshot::copy_bytes(fixture.raw, Fixture::channels, fixture.packed,
                                                   damaged, {}, Validation::OnDemand);
        require(!deferred->stats().all_payloads_verified, "on-demand policy is explicitly weaker");
        Worker worker(Query::prepare(deferred, constant_program(true)));
        (void)check_run(worker, Backend::Auto, fixture, 2, 1, all);
        Collect selected_bad;
        rejects([&] { worker.run(Backend::Auto, 1, 1, selected_bad); },
                "selected bad frame fails", "decompression failed");
        require(selected_bad.rows.empty(), "bad first selected frame emits nothing");
        Collect prefix;
        rejects([&] { worker.run(Backend::Auto, 0, 3, prefix); },
                "streaming failure after an emitted prefix", "decompression failed");
        compare_rows(prefix.rows, oracle(fixture, 0, 1, all), "failure prefix");
        Collect captured_prefix;
        std::unique_ptr<ResultCache> captured_bad;
        rejects([&]{auto result=worker.run_captured(Backend::Auto,0,3,captured_prefix,1U<<20);
                    captured_bad=std::move(result.cache);},"corrupt frame cannot become a captured result",
                "decompression failed");
        require(!captured_bad,"failed decode returns no captured candidate");
        compare_rows(captured_prefix.rows,oracle(fixture,0,1,all),"capture decode failure prefix");
        (void)check_run(worker, Backend::Auto, fixture, 2, 2, all);
        Worker skipped(Query::prepare(deferred, constant_program(false)));
        (void)check_run(skipped, Backend::Auto, fixture, 0, fixture.rows, none);

        auto bad_hash = fixture.store;
        bad_hash[64 + 24 + 16] ^= 1;
        refresh_store_metadata(bad_hash, fixture.rows);
        rejects([&] { (void)Snapshot::copy_bytes(fixture.raw, Fixture::channels, fixture.packed, bad_hash); },
                "full decoded-byte binding", "decoded checksum");
        auto deferred_hash = Snapshot::copy_bytes(fixture.raw, Fixture::channels, fixture.packed,
                                                  bad_hash, {}, Validation::OnDemand);
        Worker hash_worker(Query::prepare(deferred_hash, constant_program(true)));
        Collect output;
        rejects([&] { hash_worker.run(Backend::Auto, 1, 1, output); },
                "on-demand verifies selected decoded checksum", "decoded checksum");
        require(output.rows.empty(), "decoded checksum failure precedes callback");
    });

    group("feature pairing, range certificates, and snapshot budgets", [&] {
        auto raw = fixture.raw;
        put32(raw.data() + 4, std::bit_cast<std::uint32_t>(fixture.features[1] + 1));
        rejects([&] { (void)Snapshot::copy_bytes(raw, Fixture::channels, fixture.packed, fixture.store); },
                "raw/packed mismatch in another channel", "identity differs");
        auto packed = fixture.packed;
        put32(packed.data() + 64, 0); put32(packed.data() + 68, 0);
        std::fill(packed.begin() + 48, packed.begin() + 56, std::uint8_t{0});
        put64(packed.data() + 48, fixture_hash(std::span<const std::uint8_t>(packed).first(
            get32(packed.data() + 12))));
        rejects([&] { (void)Snapshot::copy_bytes(fixture.raw, Fixture::channels, packed, fixture.store); },
                "false range metadata cannot certify folded queries", "range");
        auto store = fixture.store; store[32] ^= 1;
        refresh_store_metadata(store, fixture.rows);
        rejects([&] { (void)Snapshot::copy_bytes(fixture.raw, Fixture::channels, fixture.packed, store); },
                "payload feature binding", "binding checksum");
        const auto input_bytes = fixture.raw.size() + fixture.packed.size() + fixture.store.size();
        const auto bound_bytes = ((fixture.rows + Fixture::block_rows - 1) / Fixture::block_rows) *
                                  Fixture::channels * sizeof(Bounds);
        Limits exact;
        exact.max_snapshot_bytes = input_bytes + bound_bytes;
        exact.max_rows = fixture.rows;
        exact.max_row_bytes = fixture.max_row_bytes();
        exact.max_total_decoded_bytes = fixture.decoded_bytes();
        const auto bounded = fixture.snapshot(Validation::Full, exact);
        require(bounded->stats().block_range_bytes == bound_bytes, "block bounds are accounted");
        auto small = exact; --small.max_snapshot_bytes;
        rejects([&] { (void)fixture.snapshot(Validation::Full, small); }, "snapshot cap includes block bounds", "budget");
        small = exact; small.max_snapshot_bytes = input_bytes - 1;
        rejects([&] { (void)fixture.snapshot(Validation::Full, small); }, "owned input budget", "budget");
        small = exact; --small.max_rows;
        rejects([&] { (void)fixture.snapshot(Validation::Full, small); }, "row budget", "budget");
        small = exact; --small.max_row_bytes;
        rejects([&] { (void)fixture.snapshot(Validation::Full, small); }, "per-row decode budget", "budget");
        small = exact; --small.max_total_decoded_bytes;
        rejects([&] { (void)fixture.snapshot(Validation::Full, small); }, "total decode budget", "budget");
        rejects([&] { (void)Snapshot::copy_bytes(fixture.raw, 0, fixture.packed, fixture.store); }, "zero channels");
        rejects([&] { (void)fixture.snapshot(static_cast<Validation>(99)); }, "invalid validation policy");
    });

    group("output admission precedes callbacks and counts empty selected rows", [&] {
        Worker worker(query);
        const auto expected = oracle(fixture, 0, fixture.rows, selected);
        const OutputLimits exact{expected.size(), total_bytes(expected)};
        (void)check_run(worker, Backend::Auto, fixture, 0, fixture.rows, selected, exact);
        Collect output;
        rejects([&] { worker.run(Backend::Auto, 0, fixture.rows, output,
                                 {exact.max_rows - 1, exact.max_bytes}); },
                "selected row cap", "output budget");
        require(output.rows.empty(), "row cap rejects before first sink callback");
        rejects([&] { worker.run(Backend::Auto, 0, fixture.rows, output,
                                 {exact.max_rows, exact.max_bytes - 1}); },
                "selected byte cap", "output budget");
        require(output.rows.empty(), "byte cap rejects before first sink callback");
        (void)check_run(worker, Backend::Auto, fixture, 0, 1, selected, {1, 0});
        rejects([&] { worker.run(Backend::Auto, 0, 1, output, {0, 0}); },
                "empty payload still consumes one output row", "output budget");
        require(output.rows.empty(), "empty selected row cap emits nothing");
        Worker false_worker(Query::prepare(snapshot, constant_program(false)));
        (void)check_run(false_worker, Backend::Auto, fixture, 0, fixture.rows, none, {0, 0});
    });

    group("empty datasets, empty payloads, constants and scratch reuse", [&] {
        Fixture empty(0);
        for (const bool value : {false, true}) {
            const auto prepared = Query::prepare(empty.snapshot(), constant_program(value));
            for (const auto backend : backends) {
                Worker worker(prepared);
                (void)check_run(worker, backend, empty, 0, 0, value ? all : none);
            }
        }
        Fixture only_empty(1);
        Worker empty_payload(Query::prepare(only_empty.snapshot(), constant_program(true)));
        (void)check_run(empty_payload, Backend::Auto, only_empty, 0, 1, all, {1, 0});
        for (const bool value : {false, true}) {
            Worker worker(Query::prepare(snapshot, constant_program(value)));
            for (const auto backend : backends) {
                (void)check_run(worker, backend, fixture, 1, 322, value ? all : none);
                (void)check_run(worker, backend, fixture, 323, 0, value ? all : none);
                (void)check_run(worker, backend, fixture, 63, 65, value ? all : none);
                (void)check_run(worker, backend, fixture, 322, 1, value ? all : none);
            }
        }
        Worker alternating(query);
        for (const auto backend : backends) {
            (void)check_run(alternating, backend, fixture, 0, 64, selected);
            (void)check_run(alternating, backend, fixture, 64, 64, selected);
            (void)check_run(alternating, backend, fixture, 0, 0, selected);
            (void)check_run(alternating, backend, fixture, 320, 3, selected);
        }
    });

    group("invalid ranges, backend requests and null owners", [&] {
        Worker worker(query);
        Collect sink;
        rejects([&] { worker.run(Backend::Auto, 324, 0, sink); }, "first beyond end");
        rejects([&] { worker.run(Backend::Auto, 1, UINT64_MAX, sink); }, "count addition overflow");
        rejects([&] { worker.run(Backend::Auto, UINT64_MAX, 0, sink); }, "first integer maximum");
        rejects([&] { worker.run(static_cast<Backend>(99), 0, 1, sink); }, "invalid backend");
        require(sink.rows.empty(), "invalid calls emit no rows");
        rejects([&] { (void)snapshot->frame(fixture.rows); }, "frame row bounds");
        rejects([&] { (void)snapshot->decoded_size(UINT64_MAX); }, "decoded length row bounds");
        rejects([&] { (void)Query::prepare({}, selection_program()); }, "null query snapshot");
        rejects([&] { Worker invalid(std::shared_ptr<const Query>{}); }, "null worker query");
        rejects([&] { Decoder invalid(std::shared_ptr<const Snapshot>{}); }, "null decoder snapshot");
        rejects([&] { (void)ResultCache::build({}, Backend::Auto, 0, 0, 0); }, "null cache query");
    });

    group("sink failure leaves an exact prefix and worker remains reusable", [&] {
        struct ThrowAfterTwo final : RowSink {
            Rows rows;
            void row(std::uint64_t id, std::span<const std::uint8_t> bytes) override {
                if (rows.size() == 2) throw std::runtime_error("intentional sink stop");
                rows.push_back({id, Bytes(bytes.begin(), bytes.end())});
            }
        } sink;
        Worker worker(Query::prepare(snapshot, constant_program(true)));
        rejects([&] { worker.run(Backend::Auto, 0, 5, sink); }, "sink exception propagation", "intentional");
        compare_rows(sink.rows, oracle(fixture, 0, 2, all), "sink failure prefix");
        (void)check_run(worker, Backend::Auto, fixture, 1, 65, all);
    });

    group("independent concurrent workers share only immutable owners", [&] {
        const auto run = [&](std::uint64_t first, std::uint64_t count) {
            Worker worker(query);
            Collect sink;
            const auto stats = worker.run(Backend::Auto, first, count, sink);
            require(stats.selected_rows == sink.rows.size(), "concurrent worker statistics");
            return sink.rows;
        };
        auto left = std::async(std::launch::async, run, 1, 192);
        auto right = std::async(std::launch::async, run, 191, 132);
        compare_rows(left.get(), oracle(fixture, 1, 192, selected), "concurrent first worker");
        compare_rows(right.get(), oracle(fixture, 191, 132, selected), "concurrent second worker");
        counts.pipeline_runs += 2;
    });

    std::cout << "PAYLOAD_TEST_COUNTS {\"groups\":" << counts.groups
              << ",\"pipeline_runs\":" << counts.pipeline_runs
              << ",\"cache_replays\":" << counts.cache_replays
              << ",\"rejections\":" << counts.rejections
              << ",\"compared_rows\":" << counts.compared_rows
              << ",\"compared_bytes\":" << counts.compared_bytes
              << ",\"feature_bound_checks\":" << counts.feature_bound_checks
              << ",\"certificate_row_checks\":" << counts.certificate_row_checks
              << ",\"zstd_version\":\"" << zstd_version() << "\"}\n";
}

// Keep the existing runtime suite and its source line numbers unchanged.
// Only the owning handle may move; partially assigned cache objects cannot
// acquire a new query identity before all of their output storage is ready.
#include <type_traits>

static_assert(!std::is_default_constructible_v<suffice::payload::ResultCache>,
              "ResultCache requires successful factory admission");
static_assert(!std::is_copy_constructible_v<suffice::payload::ResultCache>,
              "ResultCache objects must not be copied");
static_assert(!std::is_move_constructible_v<suffice::payload::ResultCache>,
              "Move the ResultCache owning handle, not its object");
static_assert(!std::is_copy_assignable_v<suffice::payload::ResultCache>,
              "Cache identity and output cannot be assigned separately");
static_assert(!std::is_move_assignable_v<suffice::payload::ResultCache>,
              "ResultCache objects must not become moved-from replay targets");
static_assert(std::is_move_constructible_v<std::unique_ptr<suffice::payload::ResultCache>>,
              "ResultCache owning handles must remain transferable");
static_assert(std::is_move_assignable_v<std::unique_ptr<suffice::payload::ResultCache>>,
              "ResultCache owning handles must remain move-assignable");
