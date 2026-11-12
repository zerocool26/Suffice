#include "suffice/wordcode.hpp"
#include "allocation_probe.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using suffice::Bounds;
using suffice::i64;
constexpr auto int_min = std::numeric_limits<std::int32_t>::min();
constexpr auto int_max = std::numeric_limits<std::int32_t>::max();

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(std::string("wordcode test: ") + message);
}
template<class Function> void must_throw(Function&& function, const char* message) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    require(failed, message);
}

struct Directory {
    std::filesystem::path path;
    Directory() {
        static std::atomic<unsigned> sequence{0};
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            path = std::filesystem::temp_directory_path() / ("suffice-wordcode-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(path)) return;
        }
        throw std::runtime_error("cannot create wordcode fixture directory");
    }
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
};

struct Fixture {
    Directory directory;
    std::vector<std::int32_t> values;
    std::uint32_t channels;
    std::unique_ptr<suffice::PackedDataset> packed;
    Fixture(std::vector<std::int32_t> input, std::uint32_t channel_count,
            std::uint32_t block_rows)
        : values(std::move(input)), channels(channel_count) {
        const auto raw = directory.path / "input.i32";
        std::ofstream stream(raw, std::ios::binary);
        for (auto value : values) {
            const auto word = std::bit_cast<std::uint32_t>(value);
            for (unsigned b = 0; b < 4; ++b)
                stream.put(static_cast<char>((word >> (8 * b)) & 255U));
        }
        stream.close();
        require(bool(stream), "cannot write fixture");
        const auto path = directory.path / "input.sfp";
        (void)suffice::pack_file(raw, path, channels, block_rows);
        packed = std::make_unique<suffice::PackedDataset>(path);
        packed->verify_payload();
    }
    std::vector<Bounds> ranges() const {
        std::vector<Bounds> ranges;
        for (const auto range : packed->info().ranges) ranges.push_back({range.min, range.max});
        return ranges;
    }
};

std::vector<std::uint64_t> original_reference(const Fixture& fixture,
                                             const suffice::CompiledProgram& plan) {
    const auto rows = fixture.packed->info().rows;
    std::vector<std::uint64_t> result(suffice::mask_words(rows));
    std::array<i64, suffice::max_program_nodes> inputs{};
    for (std::uint64_t row = 0; row < rows; ++row) {
        std::size_t count = 0;
        for (const auto& load : plan.loads())
            inputs[count++] = fixture.values[static_cast<std::size_t>(row) * fixture.channels + load.channel];
        if (plan.evaluate_exact(std::span<const i64>(inputs.data(), count)) > plan.threshold())
            result[static_cast<std::size_t>(row / 64)] |= std::uint64_t{1} << (row % 64);
    }
    return result;
}

std::vector<std::uint64_t> predicate_reference(const Fixture& fixture,
                                               const suffice::PredicatePlan& plan) {
    const auto rows = fixture.packed->info().rows;
    std::vector<std::uint64_t> result(suffice::mask_words(rows));
    std::array<bool, suffice::max_predicate_nodes> values{};
    for (std::uint64_t row = 0; row < rows; ++row) {
        for (std::size_t i = 0; i < plan.node_count; ++i) {
            const auto& node = plan.nodes[i];
            switch (node.op) {
            case suffice::PredicateOp::False: values[i] = false; break;
            case suffice::PredicateOp::True: values[i] = true; break;
            case suffice::PredicateOp::Greater:
                values[i] = fixture.values[static_cast<std::size_t>(row) * fixture.channels +
                    plan.loads[node.load_index].channel] > node.threshold;
                break;
            case suffice::PredicateOp::And: values[i] = values[node.a] && values[node.b]; break;
            case suffice::PredicateOp::Or: values[i] = values[node.a] || values[node.b]; break;
            case suffice::PredicateOp::Not: values[i] = !values[node.a]; break;
            }
        }
        if (values[plan.root]) result[static_cast<std::size_t>(row / 64)] |= std::uint64_t{1} << (row % 64);
    }
    return result;
}

std::vector<std::uint64_t> slice(std::span<const std::uint64_t> full,
                               std::uint64_t first, std::uint64_t count) {
    std::vector<std::uint64_t> result(suffice::mask_words(count));
    for (std::uint64_t i = 0; i < count; ++i)
        if ((full[static_cast<std::size_t>((first + i) / 64)] >> ((first + i) % 64)) & 1U)
            result[static_cast<std::size_t>(i / 64)] |= std::uint64_t{1} << (i % 64);
    return result;
}

void stats_check(const suffice::ExecutionStats& stats, std::span<const std::uint64_t> mask,
                 std::uint64_t rows) {
    require(stats.rows == rows, "wrong row count");
    require(stats.accepted == suffice::mask_popcount(mask), "wrong accepted count");
    require(stats.output_bytes == mask.size_bytes(), "wrong output byte count");
    require(std::accumulate(stats.resolved_by_bits.begin(), stats.resolved_by_bits.end(), std::uint64_t{0}) == rows,
            "wrong depth histogram count");
    if (rows % 64) require((mask.back() >> (rows % 64)) == 0, "nonzero output tail");
}

void check_plan(Fixture& fixture, const suffice::WordcodePlan& plan,
                std::span<const std::uint64_t> expected, bool ranges = false,
                std::size_t tile_words = 256) {
    require(plan.complete, "circuit compilation incomplete");
    suffice::WordcodeExecutor executor(*fixture.packed, plan, tile_words);
    require(executor.scratch_bytes() <= executor.required_scratch_bytes(plan, tile_words),
            "workspace estimate below allocated scratch");
    const auto rows = fixture.packed->info().rows;
    std::vector<std::uint64_t> output(expected.size(), UINT64_MAX);
    const auto stats = executor.run(output);
    require(std::equal(output.begin(), output.end(), expected.begin(), expected.end()),
            "full output differs from scalar reference");
    stats_check(stats, output, rows);
    if (executor.uses_simd() && plan.operation_count && fixture.packed->info().block_rows >= 256 &&
        rows >= 256 && tile_words >= 4)
        require(stats.kernel == "encoded-wordcode-avx2", "eligible full run did not execute AVX2 operators");
    suffice::WordcodeExecutor scalar(*fixture.packed, plan, tile_words, false);
    require(!scalar.uses_simd(), "explicit scalar path still enables SIMD");
    std::vector<std::uint64_t> scalar_output(expected.size());
    const auto scalar_stats = scalar.run(scalar_output);
    require(std::equal(scalar_output.begin(), scalar_output.end(), expected.begin(), expected.end()),
            "explicit scalar full output differs from reference");
    stats_check(scalar_stats, scalar_output, rows);
    require(stats.requested_payload_bytes <= fixture.packed->info().file_bytes - fixture.packed->info().metadata_bytes,
            "requested more source payload than complete file");
    allocation_probe::start();
    try { (void)executor.run(output); }
    catch (...) { allocation_probe::stop(); throw; }
    require(allocation_probe::stop() == 0, "prepared full execution allocated");
    if (!ranges) return;
    const std::array<std::uint64_t, 15> starts{{0,1,2,7,8,15,31,32,33,63,64,65,4095,4096,16383}};
    const std::array<std::uint64_t, 13> counts{{0,1,2,3,7,31,32,63,64,65,4097,16384,16385}};
    for (const auto first : starts) if (first <= rows) {
        for (const auto requested : counts) {
            const auto count = std::min(requested, rows - first);
            const auto wanted = slice(expected, first, count);
            std::vector<std::uint64_t> guarded(wanted.size() + 2, UINT64_MAX);
            const auto selected = std::span<std::uint64_t>(guarded).subspan(1, wanted.size());
            allocation_probe::start();
            suffice::ExecutionStats range_stats;
            try { range_stats = executor.run_range(first, count, selected); }
            catch (...) { allocation_probe::stop(); throw; }
            require(allocation_probe::stop() == 0, "prepared ranged execution allocated");
            require(guarded.front() == UINT64_MAX && guarded.back() == UINT64_MAX, "output guard overwritten");
            require(std::equal(selected.begin(), selected.end(), wanted.begin(), wanted.end()),
                    "arbitrary global range differs from reference slice");
            stats_check(range_stats, selected, count);
        }
    }
    std::array<std::uint64_t, 1> one{};
    must_throw([&] { executor.run_range(rows + 1, 0, {}); }, "invalid empty start accepted");
    must_throw([&] { executor.run_range(rows, 1, one); }, "past-end range accepted");
    must_throw([&] { executor.run_range(0, UINT64_MAX, one); }, "overflowing range accepted");
    must_throw([&] { executor.run_range(0, 0, one); }, "oversized mask accepted");
}

void check(Fixture& fixture, const suffice::CompiledProgram& source, bool ranges = false,
           std::size_t tile_words = 256) {
    const auto predicate = suffice::compile_predicate(source);
    require(predicate.complete, "test source did not lower to predicates");
    const auto plan = suffice::compile_wordcode(predicate);
    const auto expected = original_reference(fixture, source);
    check_plan(fixture, plan, expected, ranges, tile_words);
}

void exhaustive_thresholds() {
    for (const auto origin : {-128, 0, 1024}) {
        std::vector<std::int32_t> values;
        for (int i = 0; i < 256; ++i) values.push_back(origin + i);
        Fixture fixture(std::move(values), 1, 32);
        for (int threshold = origin - 1; threshold <= origin + 256; ++threshold) {
            suffice::Program source;
            source.output(source.load(0), threshold);
            check(fixture, source.compile(fixture.ranges()));
        }
    }
}

void threshold_cost_and_extremes() {
    std::vector<std::int32_t> values{int_min,int_max,-1,0,1};
    std::uint32_t state = 0x6a09e667U;
    for (unsigned i = 0; i < 2049; ++i) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        values.push_back(std::bit_cast<std::int32_t>(state));
    }
    Fixture fixture(std::move(values), 1, 4096);
    std::vector<std::int32_t> thresholds{int_min,int_min+1,int_max-1,int_max,-1,0,1,-100001,100000};
    for (unsigned bit = 0; bit < 32; ++bit) {
        thresholds.push_back(std::bit_cast<std::int32_t>(((std::uint32_t{1} << bit) - 1) ^ 0x80000000U));
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        thresholds.push_back(std::bit_cast<std::int32_t>(state));
    }
    const std::array<Bounds,1> bounds{{{int_min,int_max}}};
    for (auto threshold : thresholds) {
        suffice::Program source; source.output(source.load(0), threshold);
        const auto original = source.compile(bounds);
        const auto plan = suffice::compile_wordcode(suffice::compile_predicate(original));
        const auto biased = std::bit_cast<std::uint32_t>(threshold) ^ 0x80000000U;
        const auto essential = 32U - static_cast<unsigned>(std::countr_one(biased));
        require(plan.complete, "threshold circuit incomplete");
        require(plan.plane_count == essential, "threshold did not use exactly its essential planes");
        require(plan.operation_count == (essential ? essential-1 : 0), "single comparator binary gate bound not attained");
        require(plan.live_slot_count <= 1, "single comparator needs more than one live temporary");
        check_plan(fixture, plan, original_reference(fixture, original), threshold == -1 || threshold == 100000, 3);
    }
}

void composite_original_ir() {
    std::vector<std::int32_t> small;
    for (int x = -5; x <= 5; ++x) for (int y = -5; y <= 5; ++y) {
        small.push_back(x); small.push_back(y); small.push_back((3*x+5*y)%7);
    }
    Fixture fixture(std::move(small), 3, 32);
    for (unsigned kind = 0; kind < 8; ++kind) for (i64 threshold = -2; threshold <= 30; threshold += 2) {
        suffice::Program p;
        const auto x = p.load(0), y = p.load(1), z = p.load(2);
        auto score = x;
        switch (kind) {
        case 0: score = p.min(x,y); break;
        case 1: score = p.min(p.abs(x),p.abs(y)); break;
        case 2: score = p.max(p.square(x),p.square(y)); break;
        case 3: score = p.abs(p.max(p.mul_const(x,-3), y)); break;
        case 4: score = p.min(p.max(x,y),z); break;
        case 5: score = p.sub(p.constant(7),p.min(x,y)); break;
        case 6: score = p.max(p.min(p.abs(p.sub(p.mul_const(x,3),p.constant(7))),p.abs(p.add(y,p.constant(2)))),p.square(z)); break;
        default: score = p.mul_const(p.min(x,y),-7); break;
        }
        p.output(score,threshold);
        check(fixture,p.compile(fixture.ranges()));
    }
}

void slices_and_tiles() {
    for (const auto rows : {0U,1U,33U,65U,32771U}) {
        std::vector<std::int32_t> values(std::size_t(rows)*3);
        std::uint32_t random = 0xbb67ae85U;
        for (auto& value : values) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            value = std::bit_cast<std::int32_t>(random);
        }
        for (const auto block : {32U,4096U,65536U}) {
            Fixture fixture(values,3,block);
            suffice::Program p;
            const auto x=p.load(0),y=p.load(1),z=p.load(2);
            p.output(p.max(p.min(p.abs(x),p.abs(y)),p.sub(z,p.constant(100000000))),200000000);
            const auto source=p.compile(fixture.ranges());
            check(fixture,source,true,rows>65?256:3);
            suffice::Program signs;
            signs.output(signs.min(signs.max(signs.load(0),signs.load(1)),signs.load(2)),-1);
            check(fixture,signs.compile(fixture.ranges()),true,7);
        }
    }
}

suffice::PredicatePlan two_atoms(std::int32_t a, std::int32_t b, bool invert_a,
                                  bool invert_b, bool conjunction) {
    suffice::PredicatePlan plan;
    plan.complete=true;plan.load_count=1;plan.loads[0]={0,0,{int_min,int_max}};
    plan.nodes[0]={suffice::PredicateOp::Greater,0,0,0,a};
    plan.nodes[1]={suffice::PredicateOp::Greater,0,0,0,b};
    plan.node_count=2;
    suffice::PredicateNodeId left=0,right=1;
    if(invert_a) {left=plan.node_count++;plan.nodes[left]={suffice::PredicateOp::Not,0};}
    if(invert_b) {right=plan.node_count++;plan.nodes[right]={suffice::PredicateOp::Not,1};}
    plan.root=plan.node_count++;
    plan.nodes[plan.root]={conjunction?suffice::PredicateOp::And:suffice::PredicateOp::Or,left,right};
    return plan;
}

void sharing_and_implications() {
    std::vector<std::int32_t> values{int_min,int_max};
    for (int i=-32;i<=32;++i)values.push_back(i);
    Fixture fixture(std::move(values),1,32);
    for(int a=-3;a<=3;++a)for(int b=-3;b<=3;++b)for(unsigned mode=0;mode<8;++mode) {
        const auto predicate=two_atoms(a,b,(mode&1)!=0,(mode&2)!=0,(mode&4)!=0);
        const auto plan=suffice::compile_wordcode(predicate);
        check_plan(fixture,plan,predicate_reference(fixture,predicate));
    }
    const auto predicate=two_atoms(10,26,false,false,true);
    const auto plan=suffice::compile_wordcode(predicate);
    suffice::Program single;single.output(single.load(0),26);
    const auto strong=suffice::compile_wordcode(suffice::compile_predicate(single.compile(std::array<Bounds,1>{{{int_min,int_max}}})));
    require(plan.nodes.size()==strong.nodes.size() && plan.plane_count==strong.plane_count,
            "same-channel conjunction did not eliminate weaker comparison");

    std::uint32_t random=0x3c6ef372U;
    auto next=[&](){random^=random<<13;random^=random>>17;random^=random<<5;return random;};
    for(unsigned trial=0;trial<64;++trial) {
        suffice::PredicatePlan source;source.complete=true;source.load_count=1;
        source.loads[0]={0,0,{int_min,int_max}};
        for(unsigned i=0;i<12;++i) {
            source.nodes[source.node_count++]={suffice::PredicateOp::Greater,0,0,0,static_cast<std::int32_t>(next()%65)-32};
        }
        for(unsigned i=12;i<48;++i) {
            const auto a=static_cast<suffice::PredicateNodeId>(next()%i),b=static_cast<suffice::PredicateNodeId>(next()%i);
            const auto op=next()%3;
            source.nodes[source.node_count++]={op==0?suffice::PredicateOp::And:op==1?suffice::PredicateOp::Or:suffice::PredicateOp::Not,a,b};
        }
        source.root=source.node_count-1;
        check_plan(fixture,suffice::compile_wordcode(source),predicate_reference(fixture,source));
    }
}

void budgets_and_invalid_plans() {
    Fixture fixture({int_min,int_max,-1,0,1},1,32);
    suffice::Program p;p.output(p.load(0),0);
    const auto predicate=suffice::compile_predicate(p.compile(fixture.ranges()));
    const auto failed=suffice::compile_wordcode(predicate,1);
    require(!failed.complete && failed.nodes.empty() && failed.loads.empty() && failed.live_slot_count==0 &&
            failed.operation_count==0 && failed.plane_count==0, "partial circuit escaped budget failure");
    must_throw([&]{(void)suffice::compile_wordcode(predicate,0);},"zero budget accepted");
    must_throw([&]{(void)suffice::compile_wordcode(predicate,suffice::max_wordcode_nodes+1);},"excessive budget accepted");
    const auto valid=suffice::compile_wordcode(predicate);
    require(valid.complete,"valid circuit failed");
    must_throw([&]{suffice::WordcodeExecutor invalid(*fixture.packed,failed);},"incomplete circuit accepted");
    must_throw([&]{suffice::WordcodeExecutor invalid(*fixture.packed,valid,0);},"empty tile accepted");
    auto bad=valid;bad.nodes[bad.root].a=bad.root;
    must_throw([&]{suffice::WordcodeExecutor invalid(*fixture.packed,bad);},"cyclic circuit accepted");
    bad=valid;bad.nodes[bad.root].slot=suffice::no_wordcode_slot;
    must_throw([&]{suffice::WordcodeExecutor invalid(*fixture.packed,bad);},"invalid arena slot accepted");
    bad=valid;bad.loads[0].range={-1,1};
    must_throw([&]{suffice::WordcodeExecutor invalid(*fixture.packed,bad);},"narrow input certificate accepted");
    bad=valid;bad.loads[0].channel=1;
    must_throw([&]{suffice::WordcodeExecutor invalid(*fixture.packed,bad);},"nonexistent channel accepted");
    auto malformed=predicate;malformed.nodes[0].op=static_cast<suffice::PredicateOp>(255);
    require(!suffice::compile_wordcode(malformed).complete,"invalid predicate opcode accepted");

    // A range assumption is still needed after it makes a comparison constant.
    suffice::PredicatePlan assumed;
    assumed.complete=true;assumed.load_count=1;assumed.loads[0]={0,0,{-1,1}};
    assumed.node_count=1;assumed.root=0;
    assumed.nodes[0]={suffice::PredicateOp::Greater,0,0,0,-2};
    const auto folded=suffice::compile_wordcode(assumed);
    require(folded.complete && folded.plane_count==0 && !folded.loads.empty(),
            "range-based constant folding discarded its certificate");
    must_throw([&]{suffice::WordcodeExecutor invalid(*fixture.packed,folded);},
               "constant circuit accepted data outside its retained certificate");

    // The earlier arithmetic-to-predicate pass must preserve its own proofs too.
    suffice::Program narrow_source;
    narrow_source.output(narrow_source.load(0),-2);
    const auto narrow_compiled=narrow_source.compile(std::array<Bounds,1>{{{-1,1}}});
    const auto narrow_predicate=suffice::compile_predicate(narrow_compiled);
    require(narrow_predicate.complete && narrow_predicate.certificate_count>0,
            "upstream constant lowering discarded its original certificate");
    const auto through_pipeline=suffice::compile_wordcode(narrow_predicate);
    require(through_pipeline.complete && !through_pipeline.loads.empty(),
            "wordcode did not import upstream proof certificates");
    must_throw([&]{suffice::WordcodeExecutor invalid(*fixture.packed,through_pipeline);},
               "full lowering pipeline accepted broader input after range folding");

    // Certificates may retain an original offset; they constrain channel ranges,
    // while executable Plane loads always have zero offset. The current upstream
    // compiler may conservatively reject shifted programs before constant folding.
    suffice::PredicatePlan shifted_predicate;
    shifted_predicate.complete=true;shifted_predicate.node_count=1;
    shifted_predicate.nodes[0]={suffice::PredicateOp::False};
    shifted_predicate.certificate_count=1;
    shifted_predicate.certificates[0]={0,1,{int_min,int_max}};
    const auto shifted_circuit=suffice::compile_wordcode(shifted_predicate);
    require(shifted_circuit.complete && shifted_circuit.plane_count==0,
            "range certificate with original halo offset blocked a constant plan");
    check_plan(fixture,shifted_circuit,std::vector<std::uint64_t>(suffice::mask_words(fixture.packed->info().rows)));
}

} // namespace

void test_wordcode() {
    exhaustive_thresholds();
    threshold_cost_and_extremes();
    composite_original_ir();
    slices_and_tiles();
    sharing_and_implications();
    budgets_and_invalid_plans();
}

#ifdef SUFFICE_WORDCODE_STANDALONE
namespace {
std::atomic<bool> wordcode_tracking{false};
std::atomic<std::uint64_t> wordcode_allocations{0};
void* allocate_wordcode(std::size_t size) {
    if(wordcode_tracking.load(std::memory_order_relaxed))wordcode_allocations.fetch_add(1,std::memory_order_relaxed);
    if(auto* pointer=std::malloc(size?size:1))return pointer;
    throw std::bad_alloc();
}
}
void* operator new(std::size_t size){return allocate_wordcode(size);}
void* operator new[](std::size_t size){return allocate_wordcode(size);}
void operator delete(void* pointer)noexcept{std::free(pointer);}
void operator delete[](void* pointer)noexcept{std::free(pointer);}
void operator delete(void* pointer,std::size_t)noexcept{std::free(pointer);}
void operator delete[](void* pointer,std::size_t)noexcept{std::free(pointer);}
namespace allocation_probe {
void start()noexcept{wordcode_allocations.store(0);wordcode_tracking.store(true);}
std::uint64_t stop()noexcept{wordcode_tracking.store(false);return wordcode_allocations.load();}
}
int main() {
    try {test_wordcode();std::cout<<"PASS exact wordcode circuits, essential-plane bounds, sharing, slices, allocation and budgets\n";}
    catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
#endif
