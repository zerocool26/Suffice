#!/usr/bin/env python3
"""Compile and exercise guarded residuals and guarded carry placement.

The established 0.5 test bodies provide original-IR oracles, full/range masks,
allocation probes, source certificates, and physical block/tail coverage. This
suite adds the care-mask contract, explicit off-care differences, bounded proof
fallbacks, and source placement checks. It does not run performance timings.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
from pathlib import Path
import re

from codegen_support import ROOT, compiler_config, ensure_support, compile_program, execute, sha256
from test_codegen import FIXTURES, TEST_BODY


def replace_once(source, old, new):
    if source.count(old) != 1:
        raise RuntimeError("established test body changed at an expected integration point")
    return source.replace(old, new)


def test_body():
    body = replace_once(TEST_BODY,
        "struct ResidualBoundary { std::uint64_t must,may,exact; };",
        """struct ResidualBoundary { std::uint64_t must,may,exact,raw; bool all_lanes; };
struct VectorResidualBoundary {
    std::array<std::uint64_t,4> must,may,raw,merged;
    bool all_lanes;
};
std::uint64_t guarded_offcare_differences=0, vector_offcare_differences=0;
""")
    body = replace_once(body,
        'require(std::equal(wanted.begin(),wanted.end(),actual.begin()),"AVX2 suffix helper with unavailable prefix views differs from original IR");',
        """require(std::equal(wanted.begin(),wanted.end(),actual.merged.begin()),"AVX2 merged suffix differs from original IR");
                            for(unsigned word=0;word<4;++word) {
                                const auto care=actual.may[word]&~actual.must[word];
                                require(((actual.raw[word]^wanted[word])&care)==0,"AVX2 suffix is incorrect on care lanes");
                                if(actual.all_lanes)require(actual.raw[word]==wanted[word],"all-lane AVX2 residual lost its stronger contract");
                                else if(actual.raw[word]!=wanted[word])++vector_offcare_differences;
                            }""")
    body = replace_once(body,
        'require(b.exact==wanted,"suffix-only helper with unavailable prefix views differs from original IR");',
        """require(b.exact==wanted,"merged suffix with unavailable prefix views differs from original IR");
                            require(((b.raw^wanted)&(b.may&~b.must))==0,"scalar suffix is incorrect on care lanes");
                            if(b.all_lanes)require(b.raw==wanted,"all-lane scalar residual lost its stronger contract");
                            else if(b.raw!=wanted)++guarded_offcare_differences;""")
    return body


def specifications(smoke=False):
    lo, hi = -2147483648, 2147483647
    atomic = ("atomic", 1, lo, hi, "input x 0 0\noutput x > 123456789\n")
    conjunction = ("conjunction", 2, lo, hi, "input x 0 0\ninput y 1 0\nmin answer x y\noutput answer > 1000\n")
    disjunction = ("disjunction", 2, lo, hi, "input x 0 0\ninput y 1 0\nmax answer x y\noutput answer > 1000\n")
    overlapping = ("overlapping", 2, lo, hi, (ROOT / "examples/predicate_overlapping_gates.suf").read_text())
    mixed = ("mixed_shared", 3, lo, hi, (ROOT / "examples/predicate_mixed_three_channels.suf").read_text())
    result = []

    def add(fixture, strategy="GuardedResidual", placement="Guarded", depth=12,
            schedule="Pressure", budget=None, avx2=True, broad=False):
        result.append({"namespace": f"cg6_{len(result)}", "fixture": fixture,
                       "strategy": strategy, "placement": placement, "depth": depth,
                       "schedule": schedule, "budget": budget, "avx2": avx2, "broad": broad})

    for depth in (0, 1, 12, 31, 32):
        add(atomic, depth=depth)
    for fixture in (conjunction, disjunction, FIXTURES[2], overlapping):
        for strategy in ("Residual", "GuardedResidual"):
            for placement in ("Eager", "Guarded"):
                add(fixture, strategy, placement, broad=fixture[0] == "magnitude")
    for fixture in (mixed, FIXTURES[7], FIXTURES[8], FIXTURES[9], FIXTURES[12], FIXTURES[11]):
        add(fixture, broad=fixture[0] in ("affine", "min_coefficient", "certificate_constant"))
    for strategy in ("Residual", "GuardedResidual"):
        for placement in ("Eager", "Guarded"):
            add(FIXTURES[2], strategy, placement, schedule="Legacy")
    for budget in ({"rewrite_attempt_budget": 0}, {"proof_step_budget": 0}, {"bdd_node_budget": 2}):
        add(conjunction, budget=budget)
    add(FIXTURES[2], avx2=False, broad=True)
    if smoke:
        return [result[i] for i in (0, 2, 4, 5, 6, 7, 8, 17, 18, 19, 20, 30, 31, 32, 33, 34)]
    return result


def metadata(header):
    keys = ["encoded_guarded_residual_emitted", "encoded_guarded_carry_emitted",
            "encoded_residual_exact_all_lanes", "encoded_prefix_nodes", "encoded_prefix_planes",
            "encoded_prefix_live_values", "encoded_residual_nodes", "encoded_residual_planes",
            "encoded_residual_carries", "encoded_residual_initial_carries", "encoded_residual_live_values",
            "encoded_proof_attempts", "encoded_proof_accepted_rewrites", "encoded_proof_bdd_nodes",
            "encoded_proof_graph_budget_rejections", "encoded_proof_steps", "encoded_proof_exhausted"]
    found = {}
    for key in keys:
        match = re.search(r"\b" + key + r" = (true|false|\d+)(?:ULL)?;", header)
        if not match:
            raise RuntimeError("missing generated metadata: " + key)
        value = match.group(1)
        found[key] = value == "true" if value in ("true", "false") else int(value)
    for key in ("encoded_strategy", "encoded_carry_placement", "encoded_residual_scope", "encoded_proof_reason"):
        match = re.search(r"\b" + key + r' = ("(?:\\.|[^"\\])*");', header)
        if not match:
            raise RuntimeError("missing generated string metadata: " + key)
        found[key] = match.group(1)[1:-1]
    return found


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--compiler")
    parser.add_argument("--cache", type=Path)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--smoke", action="store_true", help="Reduced integration fixture inventory")
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
             'int main(int argc,char** argv){if(argc!=2)return 2;const std::filesystem::path output(argv[1]);']
    includes, calls = [], []
    for item in spec:
        ns = item["namespace"]
        name, channels, minimum, maximum, source = item["fixture"]
        options = (f'.namespace_name="{ns}",.enable_avx2={str(item["avx2"]).lower()},'
                   f'.encoded_strategy=suffice::EncodedStrategy::{item["strategy"]},'
                   f'.encoded_prefix_bits={item["depth"]},.encoded_schedule=suffice::EncodedSchedule::{item["schedule"]},'
                   f'.encoded_carry_placement=suffice::EncodedCarryPlacement::{item["placement"]}')
        if item["budget"]:
            options += ',.guarded_residual_options={' + ','.join('.'+k+'='+str(v) for k,v in item["budget"].items()) + '}'
        lines += [f'{{const auto plan=suffice::parse_program({json.dumps(source)}).compile(std::vector<suffice::Bounds>({channels},{{{minimum}LL,{maximum}LL}}));',
                  f'const auto code=suffice::emit_cpp(plan,{{{options}}});',
                  'if(!code.encoded_residual_emitted)throw std::runtime_error("residual fixture did not lower");',
                  f'std::ofstream file(output/"{ns}.hpp");if(!file)throw std::runtime_error("cannot write fixture");file<<code.header;}}']
        includes.append(f'#include "{ns}.hpp"')
        arguments = f'{json.dumps(source)},{channels},{minimum}LL,{maximum}LL'
        enabled = f'{ns}::encoded_avx2_emitted&&{ns}::avx2_available()&&std::endian::native==std::endian::little'
        if item["broad"]:
            calls.append(f'exercise_encoded<{ns}::PreparedEncodedKernel<suffice::PackedDataset>>({arguments},{enabled});')
        calls.append(f'exercise_frontier_focus<{ns}::PreparedEncodedKernel<suffice::PackedDataset>>({arguments},{item["depth"]},{ns}::plane_contracts,'
            f'[](const auto& planes,std::uint32_t offset,unsigned count){{'
            f'const auto evidence={ns}::evaluate_encoded_prefix_unchecked(planes,offset,count);'
            f'auto suffix=planes;for(std::size_t i=0;i<suffix.size();++i)if({ns}::plane_contracts[i].bit<{item["depth"]})suffix[i]={{}};'
            f'const auto raw={ns}::evaluate_encoded_residual_unchecked(suffix,offset,count,evidence);'
            f'return ResidualBoundary{{evidence.must,evidence.may,evidence.must|((evidence.may&~evidence.must)&raw),raw,{ns}::encoded_residual_exact_all_lanes}};}},'
            f'{ns}_boundary_avx2,{enabled});')
    generator.write_text('\n'.join(lines)+'\n}\n')
    generator_exe = output / ("emit_fixtures.exe" if config["msvc"] else "emit_fixtures")
    print("Compiling fixture emitter", flush=True)
    generator_compile = compile_program(config, generator, generator_exe, library, output / "generator_compile.log")
    env = os.environ.copy()
    if args.sanitize:
        env["ASAN_OPTIONS"] = "detect_leaks=0"
    execute([generator_exe, output], output / "generation.log", env=env)
    helpers, suffix_checks, placement_checks = [], 0, 0
    for item in spec:
        ns, depth = item["namespace"], item["depth"]
        header = (output / (ns+".hpp")).read_text()
        item["metadata"] = meta = metadata(header)
        guarded = item["strategy"] == "GuardedResidual"
        if meta["encoded_residual_exact_all_lanes"] == guarded:
            raise RuntimeError("wrong suffix semantic scope")
        if meta["encoded_residual_carries"] > meta["encoded_residual_initial_carries"]:
            raise RuntimeError("carry elimination grew retained state")
        if item["budget"] and item["budget"].get("rewrite_attempt_budget") == 0:
            if meta["encoded_proof_attempts"] or meta["encoded_proof_accepted_rewrites"] or meta["encoded_residual_carries"] != meta["encoded_residual_initial_carries"]:
                raise RuntimeError("zero rewrite budget did not retain original residual")
        contracts = [(int(c),int(b)) for c,b in re.findall(r'\{(\d+)u,(\d+)u\}', header.split(' plane_contracts{{\n',1)[1].split('\n}};',1)[0])]
        for function in ("evaluate_encoded_residual_unchecked", "evaluate_encoded_residual_avx2_unchecked"):
            if function+'(' not in header:
                continue
            body = header.split(function+'(',1)[1].split('\n}\n',1)[0]
            for index in map(int,re.findall(r'planes\[(\d+)\]',body)):
                if contracts[index][1] < depth:
                    raise RuntimeError("suffix helper reads a prefix plane")
            suffix_checks += 1
        if item["placement"] == "Guarded":
            body = header.split('inline std::uint64_t evaluate_encoded_unchecked(',1)[1].split('\n}\n',1)[0]
            if body.count('const EncodedPrefix evidence') != 2 or body.index('if(!unresolved)return must;') >= body.index('const EncodedPrefix evidence'):
                raise RuntimeError("scalar carry aggregate is not after its uncertainty branch")
            placement_checks += 1
            if 'run_encoded_avx2_unchecked(' in header:
                body = header.split('run_encoded_avx2_unchecked(',1)[1].split('\n}\n',1)[0]
                if body.index('if(!_mm256_testz_si256(unresolved,unresolved))') >= body.index('const EncodedPrefix256 bounds'):
                    raise RuntimeError("vector carry aggregate is not after its uncertainty branch")
                placement_checks += 1
        signature = (f'VectorResidualBoundary {ns}_boundary_avx2('
                     f'const std::array<std::span<const std::uint8_t>,{ns}::encoded_plane_count>& planes,std::uint32_t offset)')
        if 'evaluate_encoded_residual_avx2_unchecked(' in header:
            helpers += ['#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)',
                        '#if defined(__GNUC__) || defined(__clang__)', '__attribute__((target("avx2")))', '#endif', signature+'{',
                        f'const auto evidence={ns}::evaluate_encoded_prefix_avx2_unchecked(planes,offset);',
                        f'auto suffix=planes;for(std::size_t i=0;i<suffix.size();++i)if({ns}::plane_contracts[i].bit<{depth})suffix[i]={{}};',
                        f'const auto raw={ns}::evaluate_encoded_residual_avx2_unchecked(suffix,offset,evidence);',
                        'const auto care=_mm256_andnot_si256(evidence.must,evidence.may);',
                        'const auto merged=_mm256_or_si256(evidence.must,_mm256_and_si256(care,raw));',
                        f'VectorResidualBoundary result{{}};result.all_lanes={ns}::encoded_residual_exact_all_lanes;',
                        '_mm256_storeu_si256(reinterpret_cast<__m256i*>(result.must.data()),evidence.must);',
                        '_mm256_storeu_si256(reinterpret_cast<__m256i*>(result.may.data()),evidence.may);',
                        '_mm256_storeu_si256(reinterpret_cast<__m256i*>(result.raw.data()),raw);',
                        '_mm256_storeu_si256(reinterpret_cast<__m256i*>(result.merged.data()),merged);return result;}',
                        '#else', signature+'{(void)planes;(void)offset;throw std::runtime_error("AVX2 unavailable");}', '#endif']
        else:
            helpers.append(signature+'{(void)planes;(void)offset;throw std::runtime_error("AVX2 suffix not emitted");}')
    if not any(i['metadata']['encoded_proof_accepted_rewrites'] for i in spec):
        raise RuntimeError("no fixture exercised admitted care rewrites")
    test = output / "generated_tests.cpp"
    counters = ["mask_checks", "allocation_checks", "encoded_mask_checks", "encoded_scalar_mask_checks",
                "encoded_vector_path_checks", "certificate_rejections", "frontier_bound_checks",
                "frontier_resolved_words", "frontier_uncertain_words", "residual_boundary_checks",
                "residual_vector_boundary_checks", "guarded_offcare_differences", "vector_offcare_differences"]
    printing = 'std::cout<<"{\\\"exact\\\":true"'
    for key in counters:
        printing += f'<<",\\\"{key}\\\":"<<{key}'
    printing += '<<"}\\n";'
    test.write_text('\n'.join(includes)+'\n'+test_body()+'\n'+'\n'.join(helpers)+
        '\nint main(int argc,char** argv){try{if(argc!=2)return 2;packed_test_directory=argv[1];'
        'std::filesystem::create_directories(packed_test_directory);\n'+'\n'.join(calls)+
        '\nrequire(guarded_offcare_differences>0,"no test exposed the narrower care contract");\n'+printing+
        '\n}catch(const std::exception& e){track.store(false);std::cerr<<e.what()<<\'\\n\';return 1;}}\n')
    executable = output / ("generated_tests.exe" if config["msvc"] else "generated_tests")
    print(f"Compiling {len(spec)} generated headers", flush=True)
    test_compile = compile_program(config, test, executable, library, output / "test_compile.log", include=output)
    execution_ms = execute([executable, output / "packed_fixtures"], output / "execution.log", env=env)
    checks = json.loads((output / "execution.log").read_text().strip())
    record = {"schema_version":6, "created_utc":datetime.datetime.now(datetime.timezone.utc).isoformat(),
              "compiler":config, "shared_support_library":support, "smoke":args.smoke,
              "generated_headers":len(spec), "emitted_variants":spec,
              "generator_compile":generator_compile, "test_compile":test_compile,
              "execution_wall_ms":execution_ms, "checks":checks,
              "suffix_structure_checks":suffix_checks, "source_placement_checks":placement_checks,
              "test_script_sha256":sha256(Path(__file__)),
              "inherited_test_body_sha256":sha256(ROOT / "scripts/test_codegen.py"),
              "header_sha256":{p.name:sha256(p) for p in sorted(output.glob('*.hpp'))},
              "scope":"Compiled generated scalar/AVX2 exact outer results; original-IR oracle; both residual contracts; direct care-only/all-lane suffix checks with prefix views removed; shifted ranges/tails/small blocks; allocation probes; certificates; proof budget fallbacks. Source guard placement is not a machine-code ordering guarantee. LeakSanitizer disabled only for sanitizer mode."}
    (output / "result.json").write_text(json.dumps(record,indent=2)+'\n')
    print(json.dumps({"status":"passed","generated_headers":len(spec),"checks":checks,
                      "suffix_structure_checks":suffix_checks,"source_placement_checks":placement_checks,
                      "sanitize":args.sanitize},indent=2))


if __name__ == "__main__":
    main()
