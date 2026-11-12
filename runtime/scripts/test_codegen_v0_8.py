#!/usr/bin/env python3
"""Validate compiled grouped plane binding against original IR and per-plane views.

This is a correctness/resource suite, not a timing benchmark. It retains the
original source-independent arithmetic oracle and allocation probes, and adds
instrumented call/slot checks, malformed-layout rejection and compile-time map
validation. No fixture silently changes the unchecked Boolean kernel.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
from pathlib import Path
import re
import subprocess

from codegen_support import ROOT, compiler_config, ensure_support, compile_program, execute, sha256
from test_codegen import FIXTURES, TEST_BODY


def specifications(smoke=False):
    items = [
        (FIXTURES[2], "Residual", "Guarded", "Pressure", 12, True),
        (FIXTURES[2], "GuardedResidual", "Recompute", "BoundsFirst", 12, True),
        (FIXTURES[2], "Eager", "Eager", "Legacy", 12, True),
        (FIXTURES[2], "Frontier", "Eager", "Pressure", 12, True),
        (FIXTURES[1], "Eager", "Eager", "Pressure", 12, True),
        (FIXTURES[7], "GuardedResidual", "Guarded", "BoundsFirst", 12, True),
        (FIXTURES[8], "Residual", "Recompute", "Pressure", 12, True),
        (FIXTURES[10], "Eager", "Eager", "Legacy", 12, True),
        (FIXTURES[11], "Residual", "Recompute", "BoundsFirst", 32, True),
        (FIXTURES[12], "GuardedResidual", "Guarded", "Pressure", 12, True),
        (FIXTURES[13], "Eager", "Eager", "Pressure", 12, True),
        (FIXTURES[2], "GuardedResidual", "Recompute", "BoundsFirst", 0, True),
        (FIXTURES[2], "GuardedResidual", "Recompute", "BoundsFirst", 32, True),
        (FIXTURES[2], "GuardedResidual", "Recompute", "BoundsFirst", 12, False),
    ]
    if smoke:
        items = [items[i] for i in (0, 1, 4, 7, 9, 10, 13)]
    return [{"fixture": f, "strategy": s, "placement": p, "schedule": order,
             "depth": depth, "avx2": avx2, "pair": i}
            for i, (f, s, p, order, depth, avx2) in enumerate(items)]


BINDING_TESTS = r'''
struct BindingCall { std::uint64_t block; std::uint32_t channel, first, count; };
struct RecordingDataset {
    const suffice::PackedDataset& data;
    bool single_only=false,truncate=false,wrong_length=false;
    mutable std::uint64_t calls=0,payload_checks=0;
    mutable std::array<BindingCall,1024> recorded{};
    const auto& info()const noexcept{return data.info();}
    std::uint32_t block_length(std::uint64_t block)const {
        return data.block_length(block)+(wrong_length?1:0);
    }
    void verify_payload()const{++payload_checks;data.verify_payload();}
    auto plane_view(std::uint64_t block,std::uint32_t channel,std::uint32_t first,std::uint32_t count)const {
        if(single_only && count!=1)throw std::invalid_argument("single-plane-only adapter");
        require(calls<recorded.size(),"binding test recorder exhausted");
        recorded[calls++]={block,channel,first,count};
        const auto result=data.plane_view(block,channel,first,count);
        return truncate && !result.empty()?result.first(result.size()-1):result;
    }
    void reset()const noexcept{calls=0;}
};
std::uint64_t binding_slot_checks=0,binding_call_checks=0,binding_allocation_checks=0,
              binding_range_checks=0,binding_rejections=0,mapping_rejections=0,
              synthetic_high_channel_checks=0,binding_plan_checks=0;

template<class F> void rejects_binding(F&& f) {
    bool rejected=false;try{f();}catch(const std::invalid_argument&){rejected=true;}
    require(rejected,"malformed binding/layout was accepted");++binding_rejections;
}

template<class Spec> void exercise_binding(const char* text,std::uint32_t channels,
                                         std::int64_t minimum,std::int64_t maximum) {
    const auto original=suffice::parse_program(text).compile(std::vector<suffice::Bounds>(channels,{minimum,maximum}));
    for(const std::uint32_t block_rows:{32u,64u,256u,4096u}) {
        if(channels>8 && block_rows>256)continue;
        const std::uint64_t rows=2ULL*block_rows+17;
        std::vector<std::int32_t> values(static_cast<std::size_t>(rows*channels));
        const auto width=std::uint64_t(maximum-minimum)+1;
        std::uint64_t state=0x832ddcbfea193ULL;
        for(auto& value:values){state^=state<<13;state^=state>>7;state^=state<<17;
            value=static_cast<std::int32_t>(minimum+std::int64_t(state%width));}
        const auto base=packed_test_directory/("binding_"+std::to_string(packed_case_number++));
        const auto raw=base.string()+".i32",packed=base.string()+".sfp";
        {std::ofstream out(raw,std::ios::binary);require(bool(out),"cannot write binding fixture");
         for(const auto value:values){const auto bits=std::bit_cast<std::uint32_t>(value);
          for(unsigned byte=0;byte<4;++byte)out.put(static_cast<char>(bits>>(8*byte)));}}
        (void)suffice::pack_file(raw,packed,channels,block_rows);
        {
            suffice::PackedDataset data(packed);data.verify_payload();
            RecordingDataset recorder{data};
            typename Spec::Views per{},grouped{};
            for(std::uint64_t block=0;block<data.block_count();++block) {
                recorder.reset();allocations.store(0);track.store(true);
                const auto per_length=Spec::bind_per(recorder,block,per);
                track.store(false);require(!allocations.load(),"per-plane binder allocated");++binding_allocation_checks;
                require(recorder.calls==Spec::plane_count,"per-plane binder call count mismatch");++binding_call_checks;
                for(std::size_t i=0;i<Spec::plane_count;++i){const auto& call=recorder.recorded[i];const auto& contract=Spec::contracts[i];
                    require(call.block==block&&call.channel==contract.channel&&call.first==contract.bit&&call.count==1,
                            "per-plane binder changed original slot request order");}
                recorder.reset();allocations.store(0);track.store(true);
                const auto group_length=Spec::bind_group(recorder,block,grouped);
                track.store(false);require(!allocations.load(),"grouped binder allocated");++binding_allocation_checks;
                require(recorder.calls==Spec::group_count,"grouped binder call count mismatch");++binding_call_checks;
                require(per_length==data.block_length(block)&&group_length==per_length,"binder physical length mismatch");
                for(std::size_t g=0;g<Spec::group_count;++g){const auto& call=recorder.recorded[g];const auto& group=Spec::groups[g];
                    require(call.block==block&&call.channel==group.channel&&call.first==group.first_bit&&call.count==group.count_bits,
                            "grouped binder request differs from plan");}
                for(std::size_t i=0;i<Spec::plane_count;++i){const auto& contract=Spec::contracts[i];
                    const auto expected=data.plane_view(block,contract.channel,contract.bit,1);
                    require(per[i].data()==expected.data()&&per[i].size()==expected.size(),"per-plane view differs from storage");
                    require(grouped[i].data()==expected.data()&&grouped[i].size()==expected.size(),"grouped view changed original slot address/stride");
                    ++binding_slot_checks;}
            }
            typename Spec::template Per<RecordingDataset> per_kernel(recorder,true,false);
            typename Spec::template Group<RecordingDataset> group_kernel(recorder,true,true);
            for(const std::uint64_t first:std::array<std::uint64_t,6>{{0,1,7,63,std::uint64_t(block_rows)-1,rows}}) {
                if(first>rows)continue;
                for(const std::uint64_t count:std::array<std::uint64_t,4>{{0,std::min<std::uint64_t>(1,rows-first),std::min<std::uint64_t>(65,rows-first),rows-first}}) {
                    const auto expected=reference(original,values,rows,channels,first,count);
                    const auto accepted=std::accumulate(expected.begin(),expected.end(),std::uint64_t{0},
                        [](auto n,auto word){return n+std::popcount(word);});
                    std::vector<std::uint64_t> mask(expected.size(),UINT64_MAX);
                    const auto blocks=count?((first+count-1)/block_rows-first/block_rows+1):0;
                    recorder.reset();allocations.store(0);track.store(true);
                    const auto got_per=per_kernel.run_range(first,count,mask);
                    track.store(false);require(!allocations.load(),"prepared per-plane binding allocated");++binding_allocation_checks;
                    require(got_per==accepted&&mask==expected,"prepared per-plane binding differs from original IR");
                    require(recorder.calls==blocks*Spec::plane_count,"prepared per-plane call count mismatch");++binding_call_checks;
                    std::fill(mask.begin(),mask.end(),UINT64_MAX);
                    recorder.reset();allocations.store(0);track.store(true);
                    const auto got_group=group_kernel.run_range(first,count,mask);
                    track.store(false);require(!allocations.load(),"prepared grouped binding allocated");++binding_allocation_checks;
                    require(got_group==accepted&&mask==expected,"prepared grouped binding differs from original IR");
                    require(recorder.calls==blocks*Spec::group_count,"prepared grouped call count mismatch");++binding_call_checks;
                    binding_range_checks+=2;
                }
            }
            recorder.reset();recorder.wrong_length=true;
            rejects_binding([&]{(void)Spec::bind_per(recorder,0,per);});
            rejects_binding([&]{(void)Spec::bind_group(recorder,0,grouped);});
            require(recorder.calls==0,"malformed block geometry reached plane_view");
            recorder.wrong_length=false;
            bool bad_block=false;try{(void)Spec::bind_group(recorder,data.block_count(),grouped);}catch(const std::out_of_range&){bad_block=true;}
            require(bad_block,"out-of-range binding block accepted");++binding_rejections;
            if constexpr(Spec::plane_count>0) {
                recorder.truncate=true;
                rejects_binding([&]{(void)Spec::bind_per(recorder,0,per);});
                rejects_binding([&]{(void)Spec::bind_group(recorder,0,grouped);});
                recorder.truncate=false;recorder.single_only=true;
                (void)Spec::bind_per(recorder,0,per);
                if constexpr(Spec::group_count<Spec::plane_count)
                    rejects_binding([&]{(void)Spec::bind_group(recorder,0,grouped);});
                recorder.single_only=false;
                // A failed bind leaves scratch unspecified; a fresh valid run
                // must rebind every slot and recover without retained bad state.
                std::vector<std::uint64_t> mask(rows/64+(rows%64!=0));
                const auto expected=reference(original,values,rows,channels,0,rows);
                (void)group_kernel.run(mask);require(mask==expected,"valid execution failed after rejected binding");++binding_range_checks;
            }
        }
        std::filesystem::remove(raw);std::filesystem::remove(packed);
    }
}
'''


def parse_plan(header):
    contracts = [(int(c), int(b)) for c, b in re.findall(
        r"\{(\d+)u,(\d+)u\}", header.split(" plane_contracts{{\n", 1)[1].split("\n}};", 1)[0])]
    groups = [tuple(map(int, x)) for x in re.findall(r"\{(\d+)u,(\d+)u,(\d+)u,(\d+)u\}",
        header.split(" plane_binding_groups{{\n", 1)[1].split("\n}};", 1)[0])]
    slots = list(map(int, re.findall(r"(\d+)u", header.split(" plane_binding_slots{{", 1)[1].split("}};", 1)[0])))
    return contracts, groups, slots


def validate_plan(contracts, groups, slots):
    if sorted(slots) != list(range(len(contracts))):
        raise RuntimeError("binding slots are not a permutation")
    expanded = []
    for channel, first, count, offset in groups:
        if count <= 0 or first < 0 or first + count > 32 or offset + count > len(slots):
            raise RuntimeError("binding group bounds are invalid")
        for bit in range(count):
            slot = slots[offset + bit]
            if contracts[slot] != (channel, first + bit):
                raise RuntimeError("binding group changed a physical plane identity")
            expanded.append(slot)
    if sorted(expanded) != list(range(len(contracts))):
        raise RuntimeError("binding groups omit/duplicate a slot")
    for previous, following in zip(groups, groups[1:]):
        if previous[0] > following[0] or (previous[0] == following[0] and previous[1] + previous[2] >= following[1]):
            raise RuntimeError("binding groups are not maximal ordered required runs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compiler")
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--smoke", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    if output.exists():
        parser.error("use a fresh output directory")
    output.mkdir(parents=True)
    spec = specifications(args.smoke)
    config = compiler_config(args.compiler, args.sanitize)
    library, support = ensure_support(config, args.cache)
    generator = output / "emit_fixtures.cpp"
    lines = ['#include "suffice/codegen.hpp"', '#include <filesystem>', '#include <fstream>',
             '#include <stdexcept>', '#include <vector>',
             'int main(int argc,char**argv){if(argc!=2)return 2;const std::filesystem::path output(argv[1]);']
    includes, calls, facades = [], [], []
    for item in spec:
        index = item["pair"]
        name, channels, minimum, maximum, source = item["fixture"]
        item["namespaces"] = []
        for policy, tag in [("PerPlane", "per"), ("Grouped", "group")]:
            ns = f"cg8_{index}_{tag}"
            item["namespaces"].append(ns)
            options = (f'.namespace_name="{ns}",.enable_avx2={str(item["avx2"]).lower()},'
                f'.encoded_strategy=suffice::EncodedStrategy::{item["strategy"]},'
                f'.encoded_prefix_bits={item["depth"]},.encoded_schedule=suffice::EncodedSchedule::{item["schedule"]},'
                f'.encoded_carry_placement=suffice::EncodedCarryPlacement::{item["placement"]},'
                f'.encoded_plane_binding=suffice::EncodedPlaneBinding::{policy}')
            lines += [f'{{const auto plan=suffice::parse_program({json.dumps(source)}).compile(std::vector<suffice::Bounds>({channels},{{{minimum}LL,{maximum}LL}}));',
                f'const auto code=suffice::emit_cpp(plan,{{{options}}});',
                'if(!code.encoded_emitted)throw std::runtime_error("binding fixture did not lower");',
                f'std::ofstream out(output/"{ns}.hpp");if(!out)throw std::runtime_error("cannot write header");out<<code.header;}}']
            includes.append(f'#include "{ns}.hpp"')
            calls.append(f'exercise_encoded<{ns}::PreparedEncodedKernel<suffice::PackedDataset>>({json.dumps(source)},{channels},{minimum}LL,{maximum}LL,'
                f'{ns}::encoded_avx2_emitted&&{ns}::avx2_available()&&std::endian::native==std::endian::little);')
        per, group = item["namespaces"]
        facades.append(f'''struct BindingCase{index} {{
 using Views={per}::EncodedPlaneViews;
 template<class D>using Per={per}::PreparedEncodedKernel<D>;
 template<class D>using Group={group}::PreparedEncodedKernel<D>;
 inline static constexpr auto& contracts={per}::plane_contracts;
 inline static constexpr auto& groups={group}::plane_binding_groups;
 inline static constexpr auto plane_count={per}::encoded_plane_count,group_count={group}::encoded_binding_groups;
 static std::uint32_t bind_per(const RecordingDataset& d,std::uint64_t b,Views& p){{return {per}::bind_encoded_planes(d,b,p);}}
 static std::uint32_t bind_group(const RecordingDataset& d,std::uint64_t b,Views& p){{return {group}::bind_encoded_planes(d,b,p);}}
}};''')
        calls.append(f'exercise_binding<BindingCase{index}>({json.dumps(source)},{channels},{minimum}LL,{maximum}LL);')
    generator.write_text("\n".join(lines) + "\n}\n")
    generator_exe = output / ("emit_fixtures.exe" if config["msvc"] else "emit_fixtures")
    print("Compiling 0.8 binding fixture emitter", flush=True)
    generator_compile = compile_program(config, generator, generator_exe, library, output / "generator_compile.log")
    env = os.environ.copy()
    if args.sanitize:
        env["ASAN_OPTIONS"] = "detect_leaks=0"
    execute([generator_exe, output], output / "generation.log", env=env)
    body_checks, plan_checks = 0, 0
    for item in spec:
        per, group = item["namespaces"]
        headers = [(output / (ns + ".hpp")).read_text() for ns in (per, group)]
        plans = [parse_plan(header) for header in headers]
        for plan in plans:
            validate_plan(*plan)
            plan_checks += 1
        if plans[0] != plans[1]:
            raise RuntimeError("binding policy changed plane slot identities")
        item["plane_count"], item["group_count"] = len(plans[0][0]), len(plans[0][1])
        for function in ("evaluate_encoded_unchecked", "run_encoded_avx2_unchecked", "evaluate_encoded_prefix_unchecked",
                         "evaluate_encoded_prefix_avx2_unchecked", "evaluate_encoded_residual_unchecked",
                         "evaluate_encoded_residual_avx2_unchecked", "evaluate_encoded_repair_unchecked",
                         "evaluate_encoded_repair_avx2_unchecked"):
            if function + "(" not in headers[0]:
                continue
            bodies = [h.split(function + "(", 1)[1].split("\n}\n", 1)[0] for h in headers]
            normalized = [body.replace(ns, "SAME_NAMESPACE") for body, ns in zip(bodies, (per, group))]
            if normalized[0] != normalized[1]:
                raise RuntimeError("binding changed unchecked Boolean body: " + function)
            body_checks += 1
    first_ns = spec[0]["namespaces"][1]
    validation = f'''
void exercise_mapping_validator(){{
 using P={first_ns}::PlaneContract;using G={first_ns}::PlaneBindingGroup;
 constexpr std::array<P,4> contracts{{{{{{UINT32_MAX,31}},{{0,1}},{{0,3}},{{134217728u,31}}}}}};
 constexpr std::array<G,4> groups{{{{{{0,1,1,0}},{{0,3,1,1}},{{134217728u,31,1,2}},{{UINT32_MAX,31,1,3}}}}}};
 constexpr std::array<std::uint32_t,4> slots{{{{1,2,3,0}}}};
 static_assert({first_ns}::valid_encoded_binding_plan(contracts,groups,slots));
 require({first_ns}::valid_encoded_binding_plan(contracts,groups,slots),"high-channel/gapped map rejected");++synthetic_high_channel_checks;
 const auto reject=[&](const auto& p,const auto& g,const auto& s){{
  require(!{first_ns}::valid_encoded_binding_plan(p,g,s),"malformed mapping accepted");++mapping_rejections;}};
 auto bad_slots=slots;bad_slots[0]=4;reject(contracts,groups,bad_slots);
 bad_slots=slots;bad_slots[0]=bad_slots[1];reject(contracts,groups,bad_slots);
 auto bad_groups=groups;bad_groups[0].count_bits=0;reject(contracts,bad_groups,slots);
 bad_groups=groups;bad_groups[0].first_bit=32;reject(contracts,bad_groups,slots);
 bad_groups=groups;bad_groups[0].count_bits=32;reject(contracts,bad_groups,slots);
 bad_groups=groups;bad_groups[0].first_slot=UINT32_MAX;reject(contracts,bad_groups,slots);
 bad_groups=groups;bad_groups[0].count_bits=3;reject(contracts,bad_groups,slots);
 bad_groups=groups;bad_groups[2].channel=0;reject(contracts,bad_groups,slots);++synthetic_high_channel_checks;
 auto bad_contracts=contracts;bad_contracts[0].bit=30;reject(bad_contracts,groups,slots);
 const std::array<std::uint32_t,3> too_short{{{{1,2,3}}}};reject(contracts,groups,too_short);
 const std::array<P,0> empty_p{{}};const std::array<G,0> empty_g{{}};const std::array<std::uint32_t,0> empty_s{{}};
 require({first_ns}::valid_encoded_binding_plan(empty_p,empty_g,empty_s),"empty binding map rejected");++binding_plan_checks;
}}
'''
    test = output / "generated_tests.cpp"
    counters = ["mask_checks", "allocation_checks", "encoded_mask_checks", "encoded_scalar_mask_checks",
                "encoded_vector_path_checks", "certificate_rejections", "binding_slot_checks", "binding_call_checks",
                "binding_allocation_checks", "binding_range_checks", "binding_rejections", "mapping_rejections",
                "synthetic_high_channel_checks", "binding_plan_checks"]
    printing = 'std::cout<<"{\\\"exact\\\":true"'
    for key in counters:
        printing += f'<<",\\\"{key}\\\":"<<{key}'
    printing += '<<"}\\n";'
    test.write_text("\n".join(includes) + "\n#include <numeric>\n" + TEST_BODY + BINDING_TESTS +
        "\n" + "\n".join(facades) + "\n" + validation +
        '\nint main(int argc,char**argv){try{if(argc!=2)return 2;packed_test_directory=argv[1];std::filesystem::create_directories(packed_test_directory);\n' +
        "\n".join(calls) + "\nexercise_mapping_validator();\n" + printing +
        '\n}catch(const std::exception&e){track.store(false);std::cerr<<e.what()<<\'\\n\';return 1;}}\n')
    executable = output / ("generated_tests.exe" if config["msvc"] else "generated_tests")
    print(f"Compiling {2 * len(spec)} generated binding headers", flush=True)
    test_compile = compile_program(config, test, executable, library, output / "test_compile.log", include=output)
    execution_ms = execute([executable, output / "packed_fixtures"], output / "execution.log", env=env)
    checks = json.loads((output / "execution.log").read_text().strip())
    # Corrupt a real emitted slot index. Native compilation must reject the
    # finite binding permutation before any execution can occur.
    header = (output / (first_ns + ".hpp")).read_text()
    bad_header, changes = re.subn(r"( plane_binding_slots\{\{)\d+u", r"\g<1>4294967295u", header, count=1)
    if changes != 1:
        raise RuntimeError("no binding slot to mutate")
    (output / "invalid_binding_map.hpp").write_text(bad_header)
    invalid_source = output / "invalid_binding_map.cpp"
    invalid_source.write_text('#include "invalid_binding_map.hpp"\nint main(){}\n')
    if config["msvc"]:
        argv = [config["executable"], *config["flags"], "/Zs", str(invalid_source)]
    else:
        argv = [config["executable"], *config["flags"], "-fsyntax-only", str(invalid_source)]
    with (output / "invalid_binding_compile.log").open("w") as stream:
        invalid = subprocess.run(argv, stdout=stream, stderr=subprocess.STDOUT, cwd=output, check=False)
    if not invalid.returncode or "generated encoded plane binding map is invalid" not in (output / "invalid_binding_compile.log").read_text():
        raise RuntimeError("corrupted generated mapping was not rejected by its static assertion")
    record = {
        "schema_version": 8, "status": "PASS", "created_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "compiler": config, "shared_support_library": support, "smoke": args.smoke,
        "generated_headers": 2 * len(spec), "pairs": spec, "generator_compile": generator_compile,
        "test_compile": test_compile, "execution_wall_ms": execution_ms, "checks": checks,
        "python_binding_plan_checks": plan_checks, "unchanged_boolean_body_checks": body_checks,
        "native_invalid_mapping_compile_rejections": 1,
        "test_script_sha256": sha256(Path(__file__)), "inherited_test_body_sha256": sha256(ROOT / "scripts/test_codegen.py"),
        "header_sha256": {p.name: sha256(p) for p in sorted(output.glob("cg8_*.hpp"))},
        "scope": "Original-IR masks; forced scalar and available AVX2; ranges/tails/constant certificates; allocation probes; instrumented binding calls; per-slot addresses/short-block strides; malformed span/geometry/single-plane adapter rejection; finite map validator including gapped/high-uint32 identities; native static-assert rejection. No performance timing; LeakSanitizer disabled only in sanitizer mode.",
    }
    (output / "result.json").write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"status": "PASS", "generated_headers": 2 * len(spec), "checks": checks,
                      "unchanged_boolean_body_checks": body_checks, "sanitize": args.sanitize}, indent=2))


if __name__ == "__main__":
    main()
