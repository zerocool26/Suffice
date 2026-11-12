#include "suffice/catalog.hpp"
#include <zstd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace catalog_test_alloc {
void fail_after(long long) noexcept;
void reset() noexcept;
}
namespace {
namespace c = suffice::catalog;
namespace p = suffice::payload;
using Bytes = std::vector<std::uint8_t>;
struct Counts {
    std::uint64_t groups{}, runs{}, rejections{}, rows{}, bytes{}, allocation_failures{};
} counts;
[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error("catalog test: " + message);
}
void require(bool condition, const std::string& message) { if (!condition) fail(message); }
template<class F> void rejects(F&& body, const std::string& fragment) {
    try { body(); }
    catch (const std::bad_alloc&) { throw; }
    catch (const std::exception& error) {
        require(std::string(error.what()).find(fragment) != std::string::npos,
                "unexpected rejection: " + std::string(error.what()));
        ++counts.rejections; return;
    }
    fail("missing expected rejection: " + fragment);
}
template<class F> void group(const char* name, F&& body) {
    body(); ++counts.groups; std::cout << "PASS catalog " << name << '\n';
}
struct Temp {
    std::filesystem::path path;
    Temp() {
        static std::atomic<unsigned> sequence{};
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                ("suffice-catalog-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                 "-" + std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(path)) return;
        }
        fail("fixture directory creation");
    }
    ~Temp() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void put32(std::uint8_t* out, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
void put64(std::uint8_t* out, std::uint64_t value) {
    for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint64_t hash(std::span<const std::uint8_t> bytes) {
    std::uint64_t result = 14695981039346656037ULL;
    for (auto byte : bytes) result = (result ^ byte) * 1099511628211ULL;
    return result;
}
Bytes read(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    const auto size = stream.tellg(); require(size >= 0, "fixture read length");
    Bytes result(static_cast<std::size_t>(size)); stream.seekg(0);
    if (!result.empty()) stream.read(reinterpret_cast<char*>(result.data()), size);
    require(static_cast<bool>(stream), "fixture read"); return result;
}
Bytes compress(const Bytes& bytes) {
    Bytes result(ZSTD_compressBound(bytes.size()));
    const std::uint8_t empty = 0;
    const auto size = ZSTD_compress(result.data(), result.size(), bytes.empty() ? &empty : bytes.data(), bytes.size(), 1);
    require(!ZSTD_isError(size), "fixture compression"); result.resize(size); return result;
}
struct Fixture {
    static constexpr std::uint64_t n = 8;
    Bytes raw, packed, store;
    std::vector<Bytes> bodies;
    explicit Fixture(std::uint8_t version = 7, std::size_t body_bytes = 0) {
        Temp temp;
        raw.resize(n * 8);
        for (std::uint64_t row = 0; row < n; ++row) {
            put32(raw.data() + row * 8, static_cast<std::uint32_t>(row));
            put32(raw.data() + row * 8 + 4, version);
            Bytes body(row == 0 ? 0 : (body_bytes ? body_bytes : 17 + row * 3));
            for (std::size_t i = 0; i < body.size(); ++i)
                body[i] = static_cast<std::uint8_t>((version + row * 11 + i * 13) & 255);
            bodies.push_back(std::move(body));
        }
        {
            std::ofstream stream(temp.path / "raw", std::ios::binary);
            stream.write(reinterpret_cast<const char*>(raw.data()), static_cast<std::streamsize>(raw.size()));
            require(static_cast<bool>(stream), "fixture raw write");
        }
        (void)suffice::pack_file(temp.path / "raw", temp.path / "packed", 2, 64);
        packed = read(temp.path / "packed");
        store.resize(64 + n * 24);
        const std::string magic = "SUFPAY01";
        std::copy(magic.begin(), magic.end(), store.begin());
        put32(store.data() + 8, 1); put32(store.data() + 12, 1);
        put64(store.data() + 16, n); put32(store.data() + 24, 2); put32(store.data() + 28, 24);
        put64(store.data() + 32, hash(raw)); put64(store.data() + 40, n * 24);
        for (std::uint64_t row = 0; row < n; ++row) {
            const auto frame = compress(bodies[row]);
            auto* entry = store.data() + 64 + row * 24;
            put64(entry, store.size()); put32(entry + 8, static_cast<std::uint32_t>(frame.size()));
            put32(entry + 12, static_cast<std::uint32_t>(bodies[row].size()));
            put64(entry + 16, hash(bodies[row]));
            store.insert(store.end(), frame.begin(), frame.end());
        }
        put64(store.data() + 48, store.size());
        put64(store.data() + 56, hash(std::span<const std::uint8_t>(store).first(64 + n * 24)));
    }
    std::shared_ptr<const p::Snapshot> snapshot(p::Validation validation = p::Validation::Full) const {
        return p::Snapshot::copy_bytes(raw, 2, packed, store, {}, validation);
    }
    std::vector<c::ArtifactRow> metadata() const {
        std::vector<c::ArtifactRow> result;
        for (std::uint64_t row = 0; row < n; ++row)
            result.push_back({"src/file" + std::to_string(row) + ".txt", std::string(64, 'a'), bodies[row].size()});
        return result;
    }
    std::uint64_t selected_bytes(std::uint64_t first = 0, std::uint64_t count = n, int threshold = 2) const {
        std::uint64_t result = 0;
        for (auto row = first; row < first + count; ++row) if (static_cast<int>(row) > threshold) result += bodies[row].size();
        return result;
    }
};
const std::string selection = "input value 0 0\noutput value > 2\n";
const std::string none = "input value 0 0\noutput value > 99\n";
struct Sink : p::RowSink {
    const Fixture& fixture;
    std::vector<std::uint64_t> expected;
    std::size_t next{};
    std::function<void()> after;
    explicit Sink(const Fixture& f, std::uint64_t first = 0, std::uint64_t count = Fixture::n, int threshold = 2)
        : fixture(f) {
        for (auto row = first; row < first + count; ++row)
            if (static_cast<int>(row) > threshold) expected.push_back(row);
    }
    void row(std::uint64_t row, std::span<const std::uint8_t> bytes) override {
        require(next < expected.size() && expected[next] == row, "ordered selected row");
        require(bytes.size() == fixture.bodies[row].size() &&
                std::equal(bytes.begin(), bytes.end(), fixture.bodies[row].begin()), "every plaintext byte");
        ++next; ++counts.rows; counts.bytes += bytes.size();
        if (after) after();
    }
    void complete() { require(next == expected.size(), "all rows delivered"); }
};
c::RunReport run(c::Catalog& catalog, const c::Lease& lease, const Fixture& f,
                 c::CacheMode mode = c::CacheMode::Use, std::string_view source = selection,
                 std::uint64_t first = 0, std::uint64_t count = Fixture::n, int threshold = 2,
                 c::PopulateStrategy population = c::PopulateStrategy::Capture) {
    Sink sink(f, first, count, threshold);
    c::RunOptions options; options.cache = mode;
    options.population = population;
    auto result = catalog.run(lease, source, first, count, sink, options);
    sink.complete(); ++counts.runs; return result;
}
std::uint64_t logical_bytes(const p::Snapshot& snapshot) {
    const auto& s = snapshot.stats();
    return s.raw_feature_bytes + s.packed_feature_bytes + s.payload_store_bytes + s.block_range_bytes;
}
} // namespace

void test_catalog() {
    Fixture old_data(7), new_data(33);
    group("two projects and exact complete cached output", [&] {
        c::Catalog catalog;
        auto a = catalog.publish("alpha", 0, "g1", old_data.snapshot(), old_data.metadata());
        auto b = catalog.publish("beta", 0, "g1", new_data.snapshot(), new_data.metadata());
        require(a.revision() != b.revision() && a.project() == "alpha" && a.channels() == 2, "lease identity");
        require(a.artifacts().size() == Fixture::n && a.artifacts()[3].size == old_data.bodies[3].size(), "lease metadata");
        const auto first = run(catalog, a, old_data, c::CacheMode::Populate);
        require(first.cache_stored && !first.cache_hit && first.pipeline.output_bytes == old_data.selected_bytes(), "first cache admission");
        const auto hit = run(catalog, a, old_data);
        require(hit.cache_hit && hit.pipeline.compressed_payload_bytes == 0 && hit.pipeline.decoded_payload_bytes == 0 &&
                hit.pipeline.feature_requested_bytes == 0 && hit.pipeline.output_bytes == old_data.selected_bytes(), "cache work counters");
        require(!run(catalog, b, new_data).cache_hit, "project separation");
        const auto stats = catalog.stats();
        require(stats.active_projects == 2 && stats.live_snapshots == 2 && stats.cache_hits == 1 && stats.cache_admissions == 1,
                "global accounting");
        require(stats.plan_worker_bytes > 0, "retained worker component storage reported");
    });
    group("publication CAS, old leases and cache invalidation", [&] {
        c::Catalog catalog;
        auto old = catalog.publish("project", 0, "generation", old_data.snapshot(), old_data.metadata());
        run(catalog, old, old_data, c::CacheMode::Populate);
        auto current = catalog.publish("project", old.revision(), "generation", new_data.snapshot(), new_data.metadata());
        require(current.revision() > old.revision(), "reused labels get new revision");
        require(catalog.stats().cache_invalidations == 1 && catalog.stats().result_entries == 0, "old result invalidation");
        rejects([&] { (void)catalog.publish("project", old.revision(), "stale", old_data.snapshot(), old_data.metadata()); }, "stale");
        require(catalog.state("project").revision == current.revision(), "stale publication unchanged");
        const auto retired = run(catalog, old, old_data, c::CacheMode::Populate);
        require(retired.retired_generation && !retired.cache_stored && !retired.worker_retained &&
                catalog.stats().plan_entries == 0 && catalog.stats().worker_entries == 0 &&
                catalog.stats().result_entries == 0, "old lease executes without hidden retained owners");
        run(catalog, current, new_data);
        require(catalog.stats().retired_snapshot_bytes > 0 && catalog.stats().retired_metadata_bytes > 0, "old lease charged");
        old = {}; catalog.collect();
        require(catalog.stats().retired_snapshot_bytes == 0 && catalog.stats().retired_metadata_bytes == 0, "old owners released");
    });
    group("drop tombstones prevent absent-state ABA", [&] {
        c::Limits limits; limits.max_project_slots = 1;
        c::Catalog catalog(limits);
        auto lease = catalog.publish("project", 0, "one", old_data.snapshot(), old_data.metadata());
        catalog.drop("project", lease.revision());
        const auto tombstone = catalog.state("project");
        require(!tombstone.loaded && tombstone.revision > lease.revision(), "tombstone revision");
        rejects([&] { (void)catalog.acquire("project"); }, "not loaded");
        rejects([&] { (void)catalog.publish("project", 0, "stale", new_data.snapshot(), new_data.metadata()); }, "stale");
        rejects([&] { (void)catalog.publish("other", 0, "one", new_data.snapshot(), new_data.metadata()); }, "slot limit");
        auto current = catalog.publish("project", tombstone.revision, "new", new_data.snapshot(), new_data.metadata());
        run(catalog, current, new_data); run(catalog, lease, old_data);
        require(catalog.stats().project_slots == 1, "bounded tombstone slot");
    });
    group("pinned snapshots block capacity until all aliases release", [&] {
        auto snapshot = old_data.snapshot();
        const auto bytes = logical_bytes(*snapshot);
        c::Limits limits; limits.max_snapshot_bytes = bytes;
        c::Catalog catalog(limits);
        auto lease = catalog.publish("one", 0, "g", snapshot, old_data.metadata());
        catalog.drop("one", lease.revision());
        lease = {}; catalog.collect();
        require(catalog.stats().snapshot_bytes == bytes && catalog.stats().metadata_bytes == 0, "external snapshot alias charged");
        rejects([&] { (void)catalog.publish("two", 0, "g", new_data.snapshot(), new_data.metadata()); }, "snapshot byte budget");
        snapshot.reset(); catalog.collect();
        require(catalog.stats().snapshot_bytes == 0, "external alias released");
        auto next = catalog.publish("two", 0, "g", old_data.snapshot(), old_data.metadata());
        run(catalog, next, old_data);
    });
    group("snapshot sharing charges once; generation metadata stays distinct", [&] {
        auto snapshot = old_data.snapshot();
        c::Limits limits; limits.max_snapshot_bytes = logical_bytes(*snapshot); limits.max_live_snapshots = 1;
        c::Catalog catalog(limits);
        auto first = catalog.publish("one", 0, "g", snapshot, old_data.metadata());
        auto second = catalog.publish("two", 0, "g", snapshot, old_data.metadata());
        const auto s = catalog.stats();
        require(s.live_snapshots == 1 && s.live_generations == 2 && s.metadata_bytes > 0, "deduplicated snapshot charge");
        run(catalog, first, old_data); run(catalog, second, old_data);
    });
    group("metadata and live-generation budget reject without replacing", [&] {
        c::Catalog probe;
        auto snapshot = old_data.snapshot();
        auto first = probe.publish("same", 0, "g", snapshot, old_data.metadata());
        const auto metadata = probe.stats().metadata_bytes;
        c::Limits limits; limits.max_metadata_bytes = metadata;
        c::Catalog bounded(limits);
        auto lease = bounded.publish("same", 0, "g", snapshot, old_data.metadata());
        rejects([&] { (void)bounded.publish("same", lease.revision(), "g", snapshot, old_data.metadata()); }, "metadata byte budget");
        require(bounded.state("same").revision == lease.revision(), "metadata failure leaves current");
        c::Limits generations; generations.max_live_generations = 1;
        c::Catalog one(generations);
        auto only = one.publish("p", 0, "g", snapshot, old_data.metadata());
        rejects([&] { (void)one.publish("p", only.revision(), "g", snapshot, old_data.metadata()); }, "count limit");
    });
    group("byte-based global LRU replaces multiple victims", [&] {
        // Each selection contains five rows and 160 bytes: 280 retained bytes.
        const auto result_bytes = 5 * 24 + old_data.selected_bytes();
        c::Limits limits; limits.max_result_bytes = result_bytes * 2; limits.max_result_entries = 8;
        c::Catalog catalog(limits);
        auto a = catalog.publish("a", 0, "g", old_data.snapshot(), old_data.metadata());
        auto b = catalog.publish("b", 0, "g", old_data.snapshot(), old_data.metadata());
        auto d = catalog.publish("d", 0, "g", old_data.snapshot(), old_data.metadata());
        require(run(catalog, a, old_data, c::CacheMode::Populate).cache_stored, "cache a");
        require(run(catalog, b, old_data, c::CacheMode::Populate).cache_stored, "cache b");
        require(run(catalog, a, old_data).cache_hit, "touch a");
        require(run(catalog, d, old_data, c::CacheMode::Populate).cache_stored, "cache d");
        require(!run(catalog, b, old_data).cache_hit && run(catalog, a, old_data).cache_hit, "oldest across projects evicted");
        const auto s = catalog.stats();
        require(s.result_bytes <= limits.max_result_bytes && s.result_entries == 2 && s.cache_evictions == 1, "byte budget held");
        const std::string all = "input value 0 0\noutput value > -1\n";
        require(run(catalog, a, old_data, c::CacheMode::Populate, all, 0, 8, -1).cache_stored, "larger candidate admitted");
        require(catalog.stats().result_entries == 1 && catalog.stats().cache_evictions == 3,
                "large candidate evicts the complete two-victim set");
        require(run(catalog, a, old_data, c::CacheMode::Use, all, 0, 8, -1).cache_hit &&
                !run(catalog, a, old_data).cache_hit && !run(catalog, d, old_data).cache_hit,
                "large retained entry and both victims exact");
    });
    group("zero-byte results bounded by entry count", [&] {
        c::Limits limits; limits.max_result_bytes = 0; limits.max_result_entries = 2;
        c::Catalog catalog(limits);
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        require(run(catalog, lease, old_data, c::CacheMode::Populate, none, 0, 8, 99).cache_stored, "zero cache 1");
        require(run(catalog, lease, old_data, c::CacheMode::Populate, none, 1, 7, 99).cache_stored, "zero cache 2");
        require(run(catalog, lease, old_data, c::CacheMode::Populate, none, 2, 6, 99).cache_stored, "zero cache 3");
        const auto s = catalog.stats();
        require(s.result_bytes == 0 && s.result_entries == 2 && s.cache_evictions == 1, "zero count bound");
        require(!run(catalog, lease, old_data, c::CacheMode::Use, none, 0, 8, 99).cache_hit, "old zero entry evicted");
    });
    group("oversized results execute without retention", [&] {
        c::Limits limits; limits.max_result_bytes = 1;
        c::Catalog catalog(limits);
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        const auto report = run(catalog, lease, old_data, c::CacheMode::Populate);
        require(report.cache_oversized && !report.cache_stored && !report.cache_build_failed &&
                catalog.stats().result_entries == 0, "oversized pass through");
    });
    group("exact source and range keys; bypass; bounded plans", [&] {
        c::Limits limits; limits.max_plan_entries = 1;
        c::Catalog catalog(limits);
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        run(catalog, lease, old_data, c::CacheMode::Populate);
        require(!run(catalog, lease, old_data, c::CacheMode::Use, selection, 2, 5).cache_hit, "range key");
        const auto other_source = selection + "\n";
        require(!run(catalog, lease, old_data, c::CacheMode::Use, other_source).cache_hit, "source bytes key");
        require(!run(catalog, lease, old_data, c::CacheMode::Bypass).cache_hit, "bypass");
        require(run(catalog, lease, old_data, c::CacheMode::Bypass).plan_reused, "persistent plan/worker reuse");
        require(catalog.stats().plan_entries == 1 && catalog.stats().plan_evictions >= 2, "plan count limit");
        catalog.clear_plans(); require(catalog.stats().plan_text_bytes == 0 && catalog.stats().plan_worker_bytes == 0,
                                      "plan accounting cleared");
        require(run(catalog, lease, old_data).cache_hit, "result keeps exact query owner independently");
    });
    group("worker release preserves prepared queries and cached output", [&] {
        c::Catalog catalog;
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        const auto first = run(catalog, lease, old_data, c::CacheMode::Populate);
        require(!first.worker_reused && first.worker_retained && !first.worker_oversized,
                "new worker retained under default limits");
        const auto before = catalog.stats();
        catalog.clear_workers();
        const auto cleared = catalog.stats();
        require(cleared.plan_entries == 1 && cleared.worker_entries == 0 && cleared.plan_worker_bytes == 0 &&
                cleared.result_entries == 1 && cleared.worker_evictions == before.worker_evictions + 1,
                "worker-only clear leaves query and result owners");
        const auto hit = run(catalog, lease, old_data);
        require(hit.cache_hit && !hit.worker_reused && !hit.worker_retained &&
                catalog.stats().worker_preparations == before.worker_preparations,
                "cache replay does not acquire worker");
        const auto recreate = run(catalog, lease, old_data, c::CacheMode::Bypass);
        require(recreate.plan_reused && !recreate.worker_reused && recreate.worker_retained &&
                catalog.stats().plan_preparations == before.plan_preparations,
                "worker recreated without recompiling query");
        require(run(catalog, lease, old_data, c::CacheMode::Bypass).worker_reused,
                "existing worker reused separately from query");
        catalog.clear_plans();
        require(catalog.stats().plan_entries == 0 && catalog.stats().worker_entries == 0 &&
                catalog.stats().plan_worker_bytes == 0, "clear plans releases workers too");
    });
    group("worker entry LRU evicts storage without discarding queries", [&] {
        c::Limits limits; limits.max_worker_entries = 2;
        c::Catalog catalog(limits);
        auto a = catalog.publish("a", 0, "g", old_data.snapshot(), old_data.metadata());
        auto b = catalog.publish("b", 0, "g", old_data.snapshot(), old_data.metadata());
        auto d = catalog.publish("d", 0, "g", old_data.snapshot(), old_data.metadata());
        run(catalog, a, old_data, c::CacheMode::Bypass);
        run(catalog, b, old_data, c::CacheMode::Bypass);
        require(run(catalog, a, old_data, c::CacheMode::Bypass).worker_reused, "touch first worker");
        run(catalog, d, old_data, c::CacheMode::Bypass);
        const auto occupied = catalog.stats();
        require(occupied.plan_entries == 3 && occupied.worker_entries == 2 && occupied.worker_evictions == 1,
                "entry cap preserves all prepared queries");
        require(run(catalog, a, old_data, c::CacheMode::Bypass).worker_reused, "MRU worker preserved");
        const auto rebuilt = run(catalog, b, old_data, c::CacheMode::Bypass);
        require(rebuilt.plan_reused && !rebuilt.worker_reused && rebuilt.worker_retained &&
                catalog.stats().plan_preparations == 3, "LRU worker reconstructed under same query");
    });
    group("worker byte cap evicts complete unequal victim set", [&] {
        Fixture large_data(7, 256U << 10);
        c::Catalog probe;
        auto small = probe.publish("small", 0, "g", old_data.snapshot(), old_data.metadata());
        auto large = probe.publish("large", 0, "g", large_data.snapshot(), large_data.metadata());
        const auto small_bytes = run(probe, small, old_data, c::CacheMode::Bypass).worker_scratch_bytes;
        const auto large_bytes = run(probe, large, large_data, c::CacheMode::Bypass).worker_scratch_bytes;
        require(small_bytes > 0 && large_bytes > 2 * small_bytes, "unequal worker fixture capacities");
        c::Limits limits; limits.max_worker_bytes = large_bytes; limits.max_worker_entries = 8;
        c::Catalog catalog(limits);
        auto a = catalog.publish("a", 0, "g", old_data.snapshot(), old_data.metadata());
        auto b = catalog.publish("b", 0, "g", old_data.snapshot(), old_data.metadata());
        auto d = catalog.publish("d", 0, "g", large_data.snapshot(), large_data.metadata());
        run(catalog, a, old_data, c::CacheMode::Bypass);
        run(catalog, b, old_data, c::CacheMode::Bypass);
        require(catalog.stats().worker_entries == 2, "two small workers fit together");
        const auto admitted = run(catalog, d, large_data, c::CacheMode::Bypass);
        const auto occupied = catalog.stats();
        require(admitted.worker_retained && occupied.worker_entries == 1 && occupied.worker_evictions == 2 &&
                occupied.plan_entries == 3 && occupied.plan_worker_bytes == large_bytes,
                "large worker evicts both old workers with exact byte charge");
        require(!run(catalog, a, old_data, c::CacheMode::Bypass).worker_reused &&
                !run(catalog, b, old_data, c::CacheMode::Bypass).worker_reused,
                "evicted workers recreate while queries survive");
        require(catalog.stats().plan_worker_bytes <= limits.max_worker_bytes,
                "byte cap holds after subsequent reuse");
        limits.max_worker_bytes = 2 * small_bytes;
        c::Catalog transient(limits);
        auto x = transient.publish("x", 0, "g", old_data.snapshot(), old_data.metadata());
        auto y = transient.publish("y", 0, "g", old_data.snapshot(), old_data.metadata());
        auto z = transient.publish("z", 0, "g", large_data.snapshot(), large_data.metadata());
        run(transient, x, old_data, c::CacheMode::Bypass);
        run(transient, y, old_data, c::CacheMode::Bypass);
        const auto oversized = run(transient, z, large_data, c::CacheMode::Bypass);
        require(oversized.worker_oversized && !oversized.worker_retained && transient.stats().worker_entries == 2 &&
                run(transient, x, old_data, c::CacheMode::Bypass).worker_reused &&
                run(transient, y, old_data, c::CacheMode::Bypass).worker_reused,
                "oversized transient worker preserves both fitting old workers");
    });
    group("oversized and disabled workers execute transiently", [&] {
        for (const bool zero_entries : {false, true}) {
            c::Limits limits;
            if (zero_entries) limits.max_worker_entries = 0;
            else limits.max_worker_bytes = 0;
            c::Catalog catalog(limits);
            auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
            const auto first = run(catalog, lease, old_data, c::CacheMode::Bypass);
            const auto second = run(catalog, lease, old_data, c::CacheMode::Bypass);
            require(!first.worker_retained && !second.worker_retained && !second.worker_reused &&
                    second.plan_reused && second.worker_oversized == !zero_entries,
                    "transient worker preserves query reuse and exact output");
            const auto occupied = catalog.stats();
            require(occupied.plan_entries == 1 && occupied.worker_entries == 0 && occupied.plan_worker_bytes == 0 &&
                    occupied.worker_preparations == 2, "disabled retention retains no worker component");
        }
    });
    group("worker exception cleanup enforces byte and entry caps", [&] {
        for (const bool zero_entries : {false, true}) {
            c::Limits limits;
            if (zero_entries) limits.max_worker_entries = 0;
            else limits.max_worker_bytes = 1;
            c::Catalog catalog(limits);
            auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
            Sink sink(old_data); sink.after = [] { throw std::runtime_error("worker sink interruption"); };
            c::RunOptions options; options.cache = c::CacheMode::Populate;
            rejects([&] { (void)catalog.run(lease, selection, 0, 8, sink, options); }, "worker sink interruption");
            const auto after = catalog.stats();
            require(sink.next == 1 && after.plan_entries == 1 && after.worker_entries == 0 &&
                    after.plan_worker_bytes == 0 && after.result_entries == 0,
                    "throw after worker creation and decode still trims retained storage");
            const auto recovered = run(catalog, lease, old_data, c::CacheMode::Bypass);
            require(recovered.plan_reused && !recovered.worker_reused && !recovered.worker_retained,
                    "exact recovery from retained query after throwing call");
        }
    });
    group("worker allocation failure preserves prepared query", [&] {
        c::Catalog catalog;
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        run(catalog, lease, old_data, c::CacheMode::Bypass);
        catalog.clear_workers();
        const auto before = catalog.stats();
        Sink sink(old_data); c::RunOptions options; options.cache = c::CacheMode::Bypass;
        bool failed = false;
        catalog_test_alloc::fail_after(0);
        try { (void)catalog.run(lease, selection, 0, 8, sink, options); }
        catch (const std::bad_alloc&) { failed = true; ++counts.allocation_failures; }
        catch (...) { catalog_test_alloc::reset(); throw; }
        catalog_test_alloc::reset();
        require(failed && sink.next == 0 && catalog.stats().worker_entries == 0 &&
                catalog.stats().plan_preparations == before.plan_preparations,
                "allocation failure leaves immutable query and empty worker slot");
        const auto recovered = run(catalog, lease, old_data, c::CacheMode::Bypass);
        require(recovered.plan_reused && !recovered.worker_reused && recovered.worker_retained,
                "worker creation recovers using original query");
    });
    group("capture and recompute strategies preserve complete output", [&] {
        c::Catalog catalog;
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        for (const auto strategy : {c::PopulateStrategy::Capture, c::PopulateStrategy::Recompute}) {
            catalog.clear_results();
            const auto computed = run(catalog, lease, old_data, c::CacheMode::Populate, selection, 0, 8, 2, strategy);
            require(computed.cache_stored && !computed.cache_build_failed && !computed.cache_oversized,
                    "strategy stores complete bounded candidate");
            require(computed.execution_ms >= 0 && computed.cache_build_ms >= 0,
                    "diagnostic timing partition is nonnegative");
            require(run(catalog, lease, old_data).cache_hit, "each strategy replays exact output");
        }
    });
    group("capture candidate allocation failure preserves delivered output and prior cache", [&] {
        c::Catalog catalog;
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        run(catalog, lease, old_data, c::CacheMode::Populate);
        const std::string candidate_source = selection + "\n";
        run(catalog, lease, old_data, c::CacheMode::Bypass, candidate_source);
        const auto before = catalog.stats();
        // Warmed query/worker: these are candidate object, row-vector and body-vector allocations.
        for (long long point = 0; point < 3; ++point) {
            Sink sink(old_data); c::RunOptions options; options.cache = c::CacheMode::Populate;
            c::RunReport report;
            catalog_test_alloc::fail_after(point);
            try { report = catalog.run(lease, candidate_source, 0, 8, sink, options); }
            catch (...) { catalog_test_alloc::reset(); throw; }
            catalog_test_alloc::reset(); sink.complete(); ++counts.runs; ++counts.allocation_failures;
            require(report.plan_reused && report.worker_reused && report.cache_build_failed && !report.cache_stored &&
                    report.pipeline.output_bytes == old_data.selected_bytes(),
                    "optional allocation failure falls back before first output");
            require(catalog.stats().result_entries == before.result_entries &&
                    catalog.stats().cache_evictions == before.cache_evictions &&
                    run(catalog, lease, old_data).cache_hit, "failed candidate preserves previous cache");
        }
    });
    group("cache and miss output limits precede callbacks", [&] {
        c::Catalog catalog;
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        run(catalog, lease, old_data, c::CacheMode::Populate);
        for (const auto mode : {c::CacheMode::Use, c::CacheMode::Bypass}) {
            Sink sink(old_data); c::RunOptions options; options.cache = mode; options.output.max_bytes = 1;
            rejects([&] { (void)catalog.run(lease, selection, 0, 8, sink, options); }, "output budget");
            require(sink.next == 0, "no partial callback on byte limit");
            options.output.max_bytes = UINT64_MAX; options.output.max_rows = 0;
            rejects([&] { (void)catalog.run(lease, selection, 0, 8, sink, options); }, "output budget");
            require(sink.next == 0, "no partial callback on row limit");
        }
        run(catalog, lease, old_data);
    });
    group("sink failure and reentrancy leave reusable owners", [&] {
        c::Catalog catalog;
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        Sink failure(old_data);
        failure.after = [] { throw std::runtime_error("intentional sink failure"); };
        c::RunOptions populate; populate.cache = c::CacheMode::Populate;
        rejects([&] { (void)catalog.run(lease, selection, 0, 8, failure, populate); }, "sink failure");
        require(failure.next == 1 && catalog.stats().result_entries == 0, "failed sink cannot publish cache");
        Sink reentrant(old_data); reentrant.after = [&] { (void)catalog.stats(); };
        rejects([&] { (void)catalog.run(lease, selection, 0, 8, reentrant); }, "reentrant");
        run(catalog, lease, old_data, c::CacheMode::Populate);
        Sink cached_failure(old_data); cached_failure.after = failure.after;
        rejects([&] { (void)catalog.run(lease, selection, 0, 8, cached_failure); }, "sink failure");
        require(run(catalog, lease, old_data).cache_hit, "complete cached object survives consumer failure");
    });
    group("lease cannot cross catalogs or become empty during callbacks", [&] {
        c::Catalog a, b;
        auto lease = a.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        Sink sink(old_data);
        rejects([&] { (void)b.run(lease, selection, 0, 8, sink); }, "another catalog");
        rejects([&] { (void)a.run({}, selection, 0, 8, sink); }, "empty");
        sink.after = [&] { lease = {}; };
        (void)a.run(lease, selection, 0, 8, sink); sink.complete(); ++counts.runs;
        require(!lease, "caller lease reset during callback");
        run(a, a.acquire("p"), old_data);
    });
    group("malformed publication and run inputs rejected", [&] {
        c::Catalog catalog;
        const auto snapshot = old_data.snapshot();
        rejects([&] { (void)catalog.publish("p", 0, "g", {}, old_data.metadata()); }, "null snapshot");
        rejects([&] { (void)catalog.publish("p", 0, "g", old_data.snapshot(p::Validation::OnDemand), old_data.metadata()); }, "Full");
        rejects([&] { (void)catalog.publish("", 0, "g", snapshot, old_data.metadata()); }, "label");
        auto metadata = old_data.metadata(); metadata.pop_back();
        rejects([&] { (void)catalog.publish("p", 0, "g", snapshot, metadata); }, "row count");
        metadata = old_data.metadata(); metadata[0].size = 1;
        rejects([&] { (void)catalog.publish("p", 0, "g", snapshot, metadata); }, "size differs");
        metadata = old_data.metadata(); metadata[0].sha256 = "abc";
        rejects([&] { (void)catalog.publish("p", 0, "g", snapshot, metadata); }, "digest");
        for (const std::string path : {"../escape", "/absolute", "a\\b", "a//b", "a/./b", "a/../b", "C:drive"}) {
            metadata = old_data.metadata(); metadata[0].path = path;
            rejects([&] { (void)catalog.publish("p", 0, "g", snapshot, metadata); }, "artifact path");
        }
        metadata = old_data.metadata(); metadata[1].path = metadata[0].path;
        rejects([&] { (void)catalog.publish("p", 0, "g", snapshot, metadata); }, "unique");
        auto lease = catalog.publish("p", 0, "g", snapshot, old_data.metadata());
        Sink sink(old_data);
        rejects([&] { (void)catalog.run(lease, selection, UINT64_MAX, 1, sink); }, "range");
        rejects([&] { (void)catalog.run(lease, "", 0, 8, sink); }, "program source");
        const std::string huge(catalog.limits().max_program_bytes + 1, ' ');
        rejects([&] { (void)catalog.run(lease, huge, 0, 8, sink); }, "program source");
        c::RunOptions invalid; invalid.cache = static_cast<c::CacheMode>(99);
        rejects([&] { (void)catalog.run(lease, selection, 0, 8, sink, invalid); }, "cache mode");
        invalid = {}; invalid.backend = static_cast<p::Backend>(99);
        rejects([&] { (void)catalog.run(lease, selection, 0, 8, sink, invalid); }, "backend");
        invalid = {}; invalid.population = static_cast<c::PopulateStrategy>(99);
        rejects([&] { (void)catalog.run(lease, selection, 0, 8, sink, invalid); }, "population strategy");
        require(sink.next == 0, "invalid input no callbacks");
    });
    group("allocation failures preserve publication state and prior caches", [&] {
        for (const bool replacement : {false, true}) {
            bool reached_success = false;
            for (long long failure_point = 0; failure_point < 64; ++failure_point) {
                c::Catalog catalog;
                auto original = catalog.publish("existing", 0, "old", old_data.snapshot(), old_data.metadata());
                run(catalog, original, old_data, c::CacheMode::Populate);
                std::string project = replacement ? "existing" : "new";
                std::string generation = "candidate";
                auto snapshot = new_data.snapshot(); auto metadata = new_data.metadata();
                const auto before = catalog.stats();
                bool failed = false;
                catalog_test_alloc::fail_after(failure_point);
                try {
                    (void)catalog.publish(std::move(project), replacement ? original.revision() : 0,
                                          std::move(generation), std::move(snapshot), std::move(metadata));
                } catch (const std::bad_alloc&) { failed = true; ++counts.allocation_failures; }
                catch (...) { catalog_test_alloc::reset(); throw; }
                catalog_test_alloc::reset();
                if (failed) {
                    require(catalog.state("existing").revision == original.revision(), "allocation failure left original revision");
                    require(catalog.stats().publications == before.publications && catalog.stats().active_projects == 1 &&
                            catalog.stats().live_snapshots == 1 && catalog.stats().live_generations == 1,
                            "allocation failure did not publish accounting");
                    require(run(catalog, original, old_data).cache_hit, "allocation failure preserved prior cache");
                } else { reached_success = true; break; }
            }
            require(reached_success, "all publication allocation points enumerated");
        }
    });
    group("optional cache allocation failure preserves successful output", [&] {
        c::Catalog catalog;
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        run(catalog, lease, old_data, c::CacheMode::Populate);
        const auto old_stats = catalog.stats();
        Sink sink(old_data);
        sink.after = [&] { if (sink.next == sink.expected.size()) catalog_test_alloc::fail_after(0); };
        c::RunOptions options; options.cache = c::CacheMode::Populate;
        c::RunReport report;
        try { report = catalog.run(lease, selection + "\n", 0, 8, sink, options); }
        catch (...) { catalog_test_alloc::reset(); throw; }
        catalog_test_alloc::reset(); sink.complete(); ++counts.runs; ++counts.allocation_failures;
        require(report.cache_build_failed && !report.cache_stored && report.pipeline.output_bytes == old_data.selected_bytes(),
                "completed output survives optional retention failure");
        require(catalog.stats().result_entries == old_stats.result_entries && run(catalog, lease, old_data).cache_hit,
                "failed construction leaves previous cache intact");
    });
    group("disabled retention and zero-length ranges", [&] {
        c::Limits limits; limits.max_plan_entries = 0; limits.max_result_entries = 0;
        c::Catalog catalog(limits);
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        auto report = run(catalog, lease, old_data, c::CacheMode::Populate);
        require(!report.cache_stored && catalog.stats().plan_entries == 0 && catalog.stats().result_entries == 0,
                "retention disabled");
        run(catalog, lease, old_data, c::CacheMode::Use, selection, Fixture::n, 0);
        require(catalog.stats().runs == 2, "empty range counted");
        catalog.clear_results(); catalog.clear_plans();
    });
    group("plan text budget and selected empty-row output limit", [&] {
        c::Limits limits; limits.max_plan_text_bytes = 0;
        c::Catalog catalog(limits);
        auto lease = catalog.publish("p", 0, "g", old_data.snapshot(), old_data.metadata());
        require(!run(catalog, lease, old_data).plan_reused && !run(catalog, lease, old_data).plan_reused &&
                catalog.stats().plan_entries == 0, "text budget disables plan retention");
        const std::string all = "input value 0 0\noutput value > -1\n";
        require(run(catalog, lease, old_data, c::CacheMode::Populate, all, 0, 1, -1).cache_stored,
                "selected empty-body row cached");
        Sink sink(old_data, 0, 1, -1);
        c::RunOptions options; options.output.max_rows = 0; options.output.max_bytes = 0;
        rejects([&] { (void)catalog.run(lease, all, 0, 1, sink, options); }, "output budget");
        require(sink.next == 0, "empty body still consumes row budget before callback");
    });
    std::cout << "CATALOG_TEST_COUNTS {\"groups\":" << counts.groups << ",\"successful_runs\":" << counts.runs
              << ",\"expected_rejections\":" << counts.rejections << ",\"compared_rows\":" << counts.rows
              << ",\"compared_bytes\":" << counts.bytes << ",\"allocation_failures\":" << counts.allocation_failures << "}\n";
}
