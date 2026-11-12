#!/usr/bin/env python3
"""Compile and execute repair-width policies; correctness evidence, not timing.

Inherited generated tests compare masks to the original arithmetic IR. The new
fixture also constructs all 16 dirty-word occupancy masks, mixes resolved true,
resolved false and unresolved lanes, checks the actual prefix geometry and
directly exercises the cold word helper. Valid prepared calls are allocation
probed. A skipped AVX2 diagnostic is reported, never counted as a pass.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
from pathlib import Path

from codegen_support import ROOT, compiler_config, ensure_support, compile_program, execute, sha256
from test_codegen import FIXTURES, TEST_BODY


INVERSE = ("inverse_magnitude", 2, -2147483648, 2147483647,
    "input x 0 0\ninput y 1 0\nabs a x\nabs b y\nmin both a b\nscale inverse both -1\noutput inverse > -100000000\n")


def specifications():
    cases = []
    for strategy in ("Residual", "GuardedResidual"):
        for policy in ("Batch256", "Word64", "Hybrid"):
            cases.append((FIXTURES[2], strategy, policy, 12, True, True))
    for policy in ("Batch256", "Word64", "Hybrid"):
        cases.append((INVERSE, "GuardedResidual", policy, 12, True, True))
    cases.extend((FIXTURES[2], "GuardedResidual", "Hybrid", p, True, False) for p in (0, 32))
    cases.append((FIXTURES[2], "GuardedResidual", "Hybrid", 12, False, False))
    for fixture in (FIXTURES[10], FIXTURES[11]):
        cases.append((fixture, "GuardedResidual", "Hybrid", 12, True, False))
    return [dict(fixture=f, strategy=s, policy=p, depth=d, avx2=a, occupancy=o,
                 namespace=f"cg9_{i}") for i, (f, s, p, d, a, o) in enumerate(cases)]


OCCUPANCY = r'''
std::uint64_t occupancy_cases=0,occupancy_masks=0,dirty_word_mask_checks=0,
              direct_word_repairs=0,occupancy_vector_skips=0;

template<class Spec> void exercise_occupancy(const char* source) {
    constexpr std::uint64_t rows=16*256+17;
    constexpr std::uint32_t channels=2;
    std::vector<std::int32_t> values(rows*channels);
    for(std::uint64_t row=0;row<rows;++row) {
        const auto batch=static_cast<unsigned>(row/256);
        const auto word=static_cast<unsigned>((row%256)/64);
        const auto bit=static_cast<unsigned>(row%64);
        auto x=(bit&1)?0:1000000000;
        auto y=x;
        // Set uncertainty at both ends of a selected word; most lanes remain
        // already decided so an unmasked care residual cannot pass by accident.
        if(batch<16 && (batch&(1u<<word)) && (bit==0 || bit==31 || bit==63)) {
            x=100000000+(bit&1?1:-1);y=1000000000;
        }
        values[2*row]=x;values[2*row+1]=y;
    }
    const auto plan=suffice::parse_program(source).compile(std::vector<suffice::Bounds>(channels,
        {std::numeric_limits<std::int32_t>::min(),std::numeric_limits<std::int32_t>::max()}));
    const auto base=packed_test_directory/("occupancy_"+std::to_string(packed_case_number++));
    const auto raw=base.string()+".i32";
    {std::ofstream out(raw,std::ios::binary);require(bool(out),"cannot write occupancy input");
     for(auto value:values){const auto bits=std::bit_cast<std::uint32_t>(value);
      for(unsigned byte=0;byte<4;++byte)out.put(static_cast<char>(bits>>(8*byte)));}}
    for(const std::uint32_t block_rows:{256u,4096u}) {
        const auto packed=base.string()+"_"+std::to_string(block_rows)+".sfp";
        (void)suffice::pack_file(raw,packed,channels,block_rows);
        {
            suffice::PackedDataset data(packed);data.verify_payload();
            typename Spec::template Kernel<suffice::PackedDataset> kernel(data,true,true),scalar(data,true,false);
            for(const std::uint64_t first:{0u,1u,7u,8u,63u,64u,255u,256u,4095u,4096u,4113u}) {
                for(const auto requested:{0u,1u,63u,64u,65u,255u,256u,257u,511u,512u,4113u}) {
                    const auto count=std::min<std::uint64_t>(requested,rows-first);
                    const auto expected=reference(plan,values,rows,channels,first,count);
                    std::uint64_t n=0;for(auto word:expected)n+=std::popcount(word);
                    std::vector<std::uint64_t> output(expected.size()+2,0xdead1337acdc5678ULL);
                    const auto mask=std::span<std::uint64_t>(output).subspan(1,expected.size());
                    for(auto* k:{&kernel,&scalar}) {
                        std::fill(mask.begin(),mask.end(),UINT64_MAX);
                        allocations.store(0);track.store(true);
                        const auto got=k->run_range(first,count,mask);
                        track.store(false);require(!allocations.load(),"occupancy execution allocated");++allocation_checks;
                        require(got==n&&std::equal(mask.begin(),mask.end(),expected.begin()),"occupancy mask disagrees with arithmetic oracle");
                        require(output.front()==0xdead1337acdc5678ULL&&output.back()==0xdead1337acdc5678ULL,"occupancy output overflow");
                        ++occupancy_masks;++mask_checks;
                    }
                }
            }
            if(Spec::available()) {
                for(unsigned batch=0;batch<16;++batch) {
                    typename Spec::Views planes{};
                    Spec::bind(data,batch*256/block_rows,planes);
                    const auto expected=reference(plan,values,rows,channels,batch*256,256);
                    Spec::check(planes,batch*256%block_rows,batch,expected);
                    ++occupancy_cases;
                }
            } else ++occupancy_vector_skips;
        }
        std::filesystem::remove(packed);
    }
    std::filesystem::remove(raw);
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler")
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error("use a fresh output directory")
    output.mkdir(parents=True)
    config = compiler_config(args.compiler, args.sanitize)
    library, support = ensure_support(config, args.cache)
    cases = specifications()
    gen = ['#include "suffice/codegen.hpp"', '#include <filesystem>', '#include <fstream>',
           '#include <stdexcept>', '#include <vector>',
           'int main(int argc,char**argv){if(argc!=2)return 2;const std::filesystem::path output(argv[1]);']
    includes, calls, facades = [], [], []
    for item in cases:
        ns = item["namespace"]
        _, channels, lo, hi, source = item["fixture"]
        opts = (f'.namespace_name="{ns}",.enable_avx2={str(item["avx2"]).lower()},'
            f'.encoded_strategy=suffice::EncodedStrategy::{item["strategy"]},'
            f'.encoded_prefix_bits={item["depth"]},.encoded_schedule=suffice::EncodedSchedule::BoundsFirst,'
            '.encoded_carry_placement=suffice::EncodedCarryPlacement::Recompute,'
            '.encoded_plane_binding=suffice::EncodedPlaneBinding::Grouped,'
            f'.encoded_repair_policy=suffice::EncodedRepairPolicy::{item["policy"]}')
        gen += [f'{{const auto plan=suffice::parse_program({json.dumps(source)}).compile(std::vector<suffice::Bounds>({channels},{{{lo}LL,{hi}LL}}));',
                f'const auto code=suffice::emit_cpp(plan,{{{opts}}});',
                'if(!code.encoded_emitted)throw std::runtime_error("repair fixture did not lower");',
                f'std::ofstream out(output/"{ns}.hpp");if(!out)throw std::runtime_error("cannot write header");out<<code.header;}}']
        includes.append(f'#include "{ns}.hpp"')
        calls.append(f'exercise_encoded<{ns}::PreparedEncodedKernel<suffice::PackedDataset>>({json.dumps(source)},{channels},{lo}LL,{hi}LL,'
                     f'{ns}::encoded_avx2_emitted&&{ns}::avx2_available()&&std::endian::native==std::endian::little);')
        if item["occupancy"]:
            # Compile this whole routine for AVX2, and call it only after the
            # generated runtime capability check. No vector ABI crosses into
            # the baseline caller: only spans, integers and vectors of uint64.
            narrow = item["policy"] != "Batch256"
            narrow_checks = f'''
    require({ns}::encoded_dirty_word_mask(q)==occupancy,"word-mask extraction differs from prefix geometry");++dirty_word_mask_checks;
    const auto repaired={ns}::evaluate_encoded_word_repair_avx2_unchecked(planes,offset,bounds.must,q,occupancy);
    std::array<std::uint64_t,4> direct{{}};_mm256_storeu_si256(reinterpret_cast<__m256i*>(direct.data()),repaired);
    require(std::equal(direct.begin(),direct.end(),expected.begin()),"direct word repair lost its care merge");++direct_word_repairs;
    for(unsigned shift:{{0u,31u,63u}}){{
      std::array<std::uint64_t,4> artificial{{}};for(unsigned i=0;i<4;++i)if(occupancy&(1u<<i))artificial[i]=std::uint64_t{{1}}<<shift;
      const auto v=_mm256_loadu_si256(reinterpret_cast<const __m256i*>(artificial.data()));
      require({ns}::encoded_dirty_word_mask(v)==occupancy,"dirty-word mask used the wrong lane width");++dirty_word_mask_checks;
    }}
''' if narrow else ""
            facades.append(f'''
struct Spec_{ns} {{
  template<class D>using Kernel={ns}::PreparedEncodedKernel<D>;using Views={ns}::EncodedPlaneViews;
  static bool available(){{return {ns}::encoded_avx2_emitted&&{ns}::avx2_available()&&std::endian::native==std::endian::little;}}
  static void bind(const suffice::PackedDataset& d,std::uint64_t b,Views& p){{(void){ns}::bind_encoded_planes(d,b,p);}}
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#if defined(__GNUC__) || defined(__clang__)
  __attribute__((target("avx2")))
#endif
  static void check(const Views& planes,std::uint32_t offset,unsigned occupancy,const std::vector<std::uint64_t>& expected){{
    const auto bounds={ns}::evaluate_encoded_prefix_avx2_unchecked(planes,offset);
    const auto q=_mm256_xor_si256(bounds.must,bounds.may);
    std::array<std::uint64_t,4> words{{}};_mm256_storeu_si256(reinterpret_cast<__m256i*>(words.data()),q);
    unsigned actual=0;for(unsigned i=0;i<4;++i)if(words[i])actual|=1u<<i;
    require(actual==occupancy,"crafted fixture has wrong dirty-word occupancy");++dirty_word_mask_checks;
    {narrow_checks}
    std::array<std::uint64_t,4> output{{}};
    const auto count={ns}::run_encoded_avx2_unchecked(planes,offset,256,output.data());
    std::uint64_t n=0;for(auto w:expected)n+=std::popcount(w);
    require(count==n&&std::equal(output.begin(),output.end(),expected.begin()),"direct AVX2 policy result differs from arithmetic oracle");++mask_checks;
  }}
#else
  static void check(const Views&,std::uint32_t,unsigned,const std::vector<std::uint64_t>&){{}}
#endif
}};''')
            calls.append(f'exercise_occupancy<Spec_{ns}>({json.dumps(source)});')
    generator = output / "emit_fixtures.cpp"
    generator.write_text("\n".join(gen)+"\n}\n")
    emitter = output / ("emit_fixtures.exe" if config["msvc"] else "emit_fixtures")
    print("Compiling repair fixture emitter", flush=True)
    gen_compile = compile_program(config, generator, emitter, library, output / "generator_compile.log")
    env = os.environ.copy()
    if args.sanitize:
        env["ASAN_OPTIONS"] = "detect_leaks=0"
    execute([emitter, output], output / "generation.log", env=env)
    counters = ["mask_checks", "allocation_checks", "encoded_mask_checks", "encoded_scalar_mask_checks",
                "encoded_vector_path_checks", "certificate_rejections", "occupancy_cases", "occupancy_masks",
                "dirty_word_mask_checks", "direct_word_repairs", "occupancy_vector_skips"]
    printing = 'std::cout<<"{\\\"exact\\\":true"'
    for key in counters:
        printing += f'<<",\\\"{key}\\\":"<<{key}'
    printing += '<<"}\\n";'
    driver = output / "generated_tests.cpp"
    driver.write_text("\n".join(includes)+"\n"+TEST_BODY+OCCUPANCY+"\n"+"\n".join(facades)+
        '\nint main(int argc,char**argv){try{if(argc!=2)return 2;packed_test_directory=argv[1];std::filesystem::create_directories(packed_test_directory);\n'+
        "\n".join(calls)+"\n"+printing+
        '\n}catch(const std::exception&e){track.store(false);std::cerr<<e.what()<<\'\\n\';return 1;}}\n')
    exe = output / ("generated_tests.exe" if config["msvc"] else "generated_tests")
    print(f"Compiling {len(cases)} generated repair headers", flush=True)
    test_compile = compile_program(config, driver, exe, library, output / "test_compile.log", include=output)
    wall_ms = execute([exe, output / "packed_fixtures"], output / "execution.log", env=env)
    checks = json.loads((output / "execution.log").read_text().strip())
    result = dict(schema_version=9, status="PASS", created_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        compiler=config, shared_support_library=support, generated_headers=len(cases), cases=cases,
        generator_compile=gen_compile, test_compile=test_compile, execution_wall_ms=wall_ms, checks=checks,
        header_sha256={p.name:sha256(p) for p in sorted(output.glob("cg9_*.hpp"))},
        script_sha256=sha256(Path(__file__)), inherited_test_body_sha256=sha256(ROOT / "scripts/test_codegen.py"),
        scope="Original arithmetic IR masks; forced scalar/available AVX2, ranges, tails, constants, depth0/depth32, complementary magnitude predicate; prepared allocation probes; all 16 dirty-word occupancy masks and direct care-merged word repair; no performance conclusions; leak detection disabled only in sanitizer mode.")
    (output / "result.json").write_text(json.dumps(result, indent=2)+"\n")
    print(json.dumps({"status":"PASS","generated_headers":len(cases),"sanitize":args.sanitize,"checks":checks},indent=2))


if __name__ == "__main__":
    main()
