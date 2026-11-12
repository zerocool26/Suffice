#include "suffice/codegen.hpp"
#include "suffice/portfolio_codegen.hpp"

#include <array>
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
void require_codegen(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> void rejects_codegen(F&& f) {
    bool rejected = false;
    try { f(); } catch (const std::invalid_argument&) { rejected = true; }
    require_codegen(rejected, "invalid code-generation request was accepted");
}
std::string function_codegen(const std::string& header,const std::string& signature) {
    const auto begin=header.find(signature);
    require_codegen(begin!=std::string::npos,"expected generated helper is missing");
    return header.substr(begin,header.find("\n}\n",begin)-begin);
}
std::vector<std::uint32_t> scalar_temporaries_codegen(const std::string& body) {
    std::vector<std::uint32_t> result;
    const std::string marker="const std::uint64_t b";
    std::size_t position=0;
    while((position=body.find(marker,position))!=std::string::npos) {
        position+=marker.size();
        result.push_back(static_cast<std::uint32_t>(std::stoul(body.substr(position))));
    }
    return result;
}
}

void test_codegen() {
    using namespace suffice;
    const auto lo = std::numeric_limits<std::int32_t>::min();
    const auto hi = std::numeric_limits<std::int32_t>::max();
    const std::array<Bounds,2> ranges{{{lo,hi},{lo,hi}}};
    auto source = parse_program("input x 0 0\ninput y 1 0\nmin both x y\noutput both > -1\n");
    const auto plan = source.compile(ranges);
    const auto generated = emit_cpp(plan);
    require_codegen(generated.predicate_lowered && generated.avx2_emitted, "predicate codegen capability missing");
    require_codegen(generated.encoded_emitted && generated.wordcode_nodes != 0, "encoded codegen capability missing");
    require_codegen(generated.encoded_avx2_emitted, "encoded AVX2 capability missing");
    require_codegen(generated.source_nodes == 3 && generated.predicate_nodes == 3, "codegen node metadata mismatch");
    require_codegen(generated.header.find("class PreparedKernel") != std::string::npos, "generated API missing");
    require_codegen(generated.header.find("evaluate_simd") != std::string::npos, "generated SIMD missing");
    require_codegen(generated.header.find("run_encoded_avx2_unchecked") != std::string::npos, "fused encoded SIMD body missing");
    require_codegen(generated.header.find("evaluate_exact_unchecked") == std::string::npos, "generated code retained IR dispatch");
    require_codegen(generated.header == emit_cpp(plan).header, "code generation is nondeterministic");
    require_codegen(!generated.encoded_frontier_emitted && generated.encoded_prefix_nodes==0,
                    "legacy eager default unexpectedly selected a frontier");
    const auto pressure=emit_cpp(plan,{.namespace_name="pressure_kernel",.encoded_schedule=EncodedSchedule::Pressure});
    require_codegen(pressure.encoded_emitted && pressure.header.find("encoded_schedule = \"pressure\"")!=std::string::npos,
                    "pressure schedule metadata missing");
    const auto planar=emit_cpp(plan,{.namespace_name="planar_kernel",.raw_layout=RawLayout::Planar});
    require_codegen(planar.header.find("raw_planar = true")!=std::string::npos,
                    "planar raw layout option ignored");
    for(const auto depth:{0u,1u,8u,12u,16u,31u,32u}) {
        const auto frontier=emit_cpp(plan,{.namespace_name="frontier_kernel",
            .encoded_strategy=EncodedStrategy::Frontier,.encoded_prefix_bits=depth,
            .encoded_schedule=EncodedSchedule::Pressure});
        require_codegen(frontier.encoded_frontier_emitted && frontier.encoded_prefix_bits==depth,
                        "frontier capability metadata missing");
        require_codegen(frontier.header.find("evaluate_encoded_prefix_unchecked")!=std::string::npos,
                        "frontier diagnostic prefix API missing");
        require_codegen(frontier.header.find("_mm256_testz_si256")!=std::string::npos,
                        "frontier exact refinement guard missing");
        require_codegen(frontier.header==emit_cpp(plan,{.namespace_name="frontier_kernel",
            .encoded_strategy=EncodedStrategy::Frontier,.encoded_prefix_bits=depth,
            .encoded_schedule=EncodedSchedule::Pressure}).header,"frontier code generation is nondeterministic");
    }
    const auto residual_source=parse_program("input x 0 0\ninput y 1 0\nabs a x\nabs b y\nmin both a b\noutput both > 123456789\n").compile(ranges);
    for(const auto strategy:{EncodedStrategy::Eager,EncodedStrategy::Frontier,EncodedStrategy::Residual,EncodedStrategy::GuardedResidual}) {
        CodegenOptions binding_options{.namespace_name="binding_kernel",.encoded_strategy=strategy,
            .encoded_prefix_bits=12,.encoded_schedule=EncodedSchedule::BoundsFirst};
        if(strategy==EncodedStrategy::Residual || strategy==EncodedStrategy::GuardedResidual)
            binding_options.encoded_carry_placement=EncodedCarryPlacement::Recompute;
        const auto per_plane=emit_cpp(residual_source,binding_options);
        binding_options.encoded_plane_binding=EncodedPlaneBinding::Grouped;
        const auto grouped=emit_cpp(residual_source,binding_options);
        require_codegen(grouped.encoded_grouped_binding_emitted && !per_plane.encoded_grouped_binding_emitted,
                        "plane binding capability metadata disagrees with requested policy");
        require_codegen(grouped.encoded_plane_count==64 && grouped.encoded_binding_groups==2 &&
                        grouped.encoded_binding_calls_per_block==2 && per_plane.encoded_binding_calls_per_block==64,
                        "complete two-channel grouping lost its checked-call reduction");
        require_codegen(grouped.header.find("encoded_plane_binding = \"grouped\"")!=std::string::npos &&
                        grouped.header.find("plane_binding_slots")!=std::string::npos &&
                        grouped.header.find("bind_encoded_planes(data_,block,planes_)")!=std::string::npos,
                        "grouped prepared execution does not use the shared binding operation");
        require_codegen(grouped.header==emit_cpp(residual_source,binding_options).header,
                        "group binding generation is nondeterministic");
        for(const auto* signature:{"inline std::uint64_t evaluate_encoded_unchecked(",
                "inline std::uint64_t run_encoded_avx2_unchecked(",
                "inline std::uint64_t evaluate_encoded_residual_unchecked(",
                "inline __m256i evaluate_encoded_residual_avx2_unchecked(",
                "inline std::uint64_t evaluate_encoded_repair_unchecked(",
                "inline __m256i evaluate_encoded_repair_avx2_unchecked("})
            if(per_plane.header.find(signature)!=std::string::npos)
                require_codegen(function_codegen(per_plane.header,signature)==function_codegen(grouped.header,signature),
                                "plane binding changed an unchecked Boolean body");
    }
    for(unsigned depth=0;depth<=32;++depth)for(const auto schedule:{EncodedSchedule::Legacy,EncodedSchedule::Pressure}) {
        const CodegenOptions options{.namespace_name="residual_kernel",.encoded_strategy=EncodedStrategy::Residual,
            .encoded_prefix_bits=depth,.encoded_schedule=schedule};
        const auto residual=emit_cpp(residual_source,options);
        require_codegen(residual.encoded_residual_emitted && !residual.encoded_frontier_emitted && residual.encoded_prefix_bits==depth,
                        "residual strategy metadata missing");
        require_codegen(residual.encoded_residual_nodes!=0 && residual.header.find("evaluate_encoded_residual_unchecked")!=std::string::npos,
                        "scalar suffix residual missing");
        require_codegen(residual.header.find("evaluate_encoded_residual_avx2_unchecked")!=std::string::npos,
                        "AVX2 suffix residual missing");
        require_codegen(residual.header.find("evaluate_encoded_exact_unchecked")==std::string::npos &&
                        residual.header.find("evaluate_encoded_exact_avx2_unchecked")==std::string::npos,
                        "residual generator silently retained complete exact fallback");
        require_codegen(residual.header==emit_cpp(residual_source,options).header,"residual emission is nondeterministic");
        if(depth==32)require_codegen(residual.encoded_residual_planes==0,"depth32 residual retains suffix reads");
        if(depth==0)require_codegen(residual.encoded_prefix_planes==0 && residual.encoded_residual_carries==0,
                                   "depth0 residual retains nonconstant prefix evidence");
    }
    for(const auto strategy:{EncodedStrategy::Residual,EncodedStrategy::GuardedResidual})
    for(const auto placement:{EncodedCarryPlacement::Eager,EncodedCarryPlacement::Guarded,EncodedCarryPlacement::Recompute})
    for(const auto schedule:{EncodedSchedule::Legacy,EncodedSchedule::Pressure,EncodedSchedule::BoundsFirst}) {
        const CodegenOptions options{.namespace_name="care_kernel",.encoded_strategy=strategy,
            .encoded_prefix_bits=12,.encoded_schedule=schedule,.encoded_carry_placement=placement};
        const auto kernel=emit_cpp(residual_source,options);
        const bool guarded=strategy==EncodedStrategy::GuardedResidual;
        require_codegen(kernel.encoded_residual_emitted && kernel.encoded_guarded_residual_emitted==guarded,
                        "guarded residual capability metadata mismatch");
        require_codegen(kernel.encoded_residual_exact_all_lanes==!guarded,
                        "guarded suffix contract was presented as all-lane exact");
        require_codegen(kernel.encoded_residual_carries<=kernel.encoded_residual_initial_carries,
                        "guarded compilation increased retained carry count");
        require_codegen(kernel.encoded_guarded_carry_emitted==(placement==EncodedCarryPlacement::Guarded),
                        "guarded carry placement metadata mismatch");
        require_codegen(kernel.encoded_recompute_carry_emitted==(placement==EncodedCarryPlacement::Recompute),
                        "recompute carry placement metadata mismatch");
        require_codegen(kernel.encoded_common_nodes>0 && kernel.encoded_common_planes>0,
                        "common source metrics are absent");
        for(const auto& [name,value]:std::array<std::pair<const char*,std::uint32_t>,6>{{
            {"encoded_common_nodes",kernel.encoded_common_nodes},{"encoded_common_planes",kernel.encoded_common_planes},
            {"encoded_common_live_values",kernel.encoded_common_live_values},
            {"encoded_cold_prefix_nodes",kernel.encoded_cold_prefix_nodes},{"encoded_cold_prefix_planes",kernel.encoded_cold_prefix_planes},
            {"encoded_cold_prefix_live_values",kernel.encoded_cold_prefix_live_values}}})
            require_codegen(kernel.header.find(std::string(name)+" = "+std::to_string(value)+";")!=std::string::npos,
                            "header and host source metrics disagree");
        require_codegen(kernel.header.find(guarded?"encoded_residual_scope = \"unresolved-lanes\"":
                                                 "encoded_residual_scope = \"all-lanes\"")!=std::string::npos,
                        "generated suffix scope metadata missing");
        if(placement==EncodedCarryPlacement::Guarded) {
            const auto outer=kernel.header.find("inline std::uint64_t evaluate_encoded_unchecked(");
            const auto end=kernel.header.find("\n}\n",outer);
            const auto body=kernel.header.substr(outer,end-outer);
            require_codegen(body.find("if(!unresolved)return must;")<body.find("const EncodedPrefix evidence"),
                            "scalar carry aggregate is not guarded in source");
            require_codegen(body.find("evaluate_encoded_prefix_unchecked(")==std::string::npos,
                            "guarded source still materializes complete prefix result eagerly");
            const auto vector_outer=kernel.header.find("inline std::uint64_t run_encoded_avx2_unchecked(");
            const auto vector_end=kernel.header.find("\n}\n",vector_outer);
            const auto vector_body=kernel.header.substr(vector_outer,vector_end-vector_outer);
            require_codegen(vector_body.find("if(!_mm256_testz_si256(unresolved,unresolved))")<
                            vector_body.find("const EncodedPrefix256 bounds"),
                            "vector carry aggregate is not guarded in source");
        }
        if(placement==EncodedCarryPlacement::Recompute) {
            const auto body=function_codegen(kernel.header,"inline std::uint64_t evaluate_encoded_unchecked(");
            require_codegen(body.find("EncodedPrefix")==std::string::npos && body.find("carries")==std::string::npos,
                            "recompute common path retains a carry aggregate");
            require_codegen(body.find("if(!unresolved)return must;")<body.find("evaluate_encoded_repair_unchecked("),
                            "scalar reconstruction is not reached through the uncertainty branch");
            require_codegen(kernel.header.find("_NOINLINE inline std::uint64_t evaluate_encoded_repair_unchecked(")!=std::string::npos,
                            "scalar reconstruction helper lost its non-inlining boundary");
            const auto vector_body=function_codegen(kernel.header,"inline std::uint64_t run_encoded_avx2_unchecked(");
            require_codegen(vector_body.find("EncodedPrefix")==std::string::npos && vector_body.find("carries")==std::string::npos,
                            "recompute vector loop retains a carry aggregate");
            require_codegen(vector_body.find("if(!_mm256_testz_si256(unresolved,unresolved))")<
                            vector_body.find("evaluate_encoded_repair_avx2_unchecked("),
                            "vector reconstruction is not reached through the uncertainty branch");
            require_codegen(kernel.header.find("_NOINLINE inline __m256i evaluate_encoded_repair_avx2_unchecked(")!=std::string::npos,
                            "vector reconstruction helper lost its non-inlining boundary");
            require_codegen(kernel.encoded_cold_prefix_nodes>0 && kernel.encoded_cold_prefix_planes>0,
                            "cold reconstruction graph metrics are absent");
        }
        if(schedule==EncodedSchedule::BoundsFirst) {
            // The bound traversal must be the same when no carry roots are
            // supplied at all. This catches a partitioned carry-driven DFS.
            auto bounds_options=options;bounds_options.encoded_carry_placement=EncodedCarryPlacement::Recompute;
            const auto reference=emit_cpp(residual_source,bounds_options);
            auto full=function_codegen(kernel.header,"inline EncodedPrefix evaluate_encoded_prefix_unchecked(");
            full.resize(full.find("        return "));
            auto common=function_codegen(reference.header,"inline std::uint64_t evaluate_encoded_unchecked(");
            common.resize(common.find("if(!unresolved)return must;"));
            const auto whole_order=scalar_temporaries_codegen(full),bound_order=scalar_temporaries_codegen(common);
            require_codegen(whole_order.size()>=bound_order.size() &&
                            std::equal(bound_order.begin(),bound_order.end(),whole_order.begin()),
                            "bound-root traversal was influenced by carry roots");
        }
        require_codegen(kernel.header==emit_cpp(residual_source,options).header,"guarded generation is nondeterministic");
    }
    const auto budget_fallback=emit_cpp(residual_source,{.namespace_name="budget_fallback",
        .encoded_strategy=EncodedStrategy::GuardedResidual,.encoded_prefix_bits=12,
        .encoded_schedule=EncodedSchedule::Pressure,.encoded_carry_placement=EncodedCarryPlacement::Guarded,
        .guarded_residual_options={.rewrite_attempt_budget=0}});
    require_codegen(budget_fallback.encoded_emitted && budget_fallback.encoded_guarded_residual_emitted &&
                    budget_fallback.encoded_proof_attempts==0 && budget_fallback.encoded_proof_accepted_rewrites==0 &&
                    budget_fallback.encoded_residual_carries==budget_fallback.encoded_residual_initial_carries,
                    "disabled rewrite budget lost valid original residual");
    for(const auto strategy:{EncodedStrategy::Residual,EncodedStrategy::GuardedResidual})
    for(const auto policy:{EncodedRepairPolicy::Batch256,EncodedRepairPolicy::Word64,EncodedRepairPolicy::Hybrid}) {
        CodegenOptions narrow{.namespace_name="narrow_kernel",.encoded_strategy=strategy,
            .encoded_prefix_bits=12,.encoded_schedule=EncodedSchedule::BoundsFirst,
            .encoded_carry_placement=EncodedCarryPlacement::Recompute,
            .encoded_plane_binding=EncodedPlaneBinding::Grouped,.encoded_repair_policy=policy};
        const auto compiled=emit_cpp(residual_source,narrow);
        const bool enabled=policy!=EncodedRepairPolicy::Batch256;
        require_codegen(compiled.encoded_narrow_repair_emitted==enabled,"narrow repair metadata mismatch");
        require_codegen((compiled.header.find("encoded_dirty_word_mask(__m256i")!=std::string::npos)==enabled,
                        "narrow repair dirty-word helper mismatch");
        if(enabled) {
            const auto body=function_codegen(compiled.header,"inline std::uint64_t run_encoded_avx2_unchecked(");
            require_codegen(body.find("encoded_dirty_word_mask(unresolved)")!=std::string::npos &&
                            body.find("evaluate_encoded_word_repair_avx2_unchecked(")!=std::string::npos,
                            "narrow repair dispatch is missing");
            require_codegen((body.find("if((dirty_words&(dirty_words-1u))==0)")!=std::string::npos)==
                            (policy==EncodedRepairPolicy::Hybrid),"hybrid dirty-word threshold is wrong");
        }
        auto scalar_options=narrow;scalar_options.enable_avx2=false;
        const auto scalar_kernel=emit_cpp(residual_source,scalar_options);
        require_codegen(!scalar_kernel.encoded_narrow_repair_emitted,"scalar-only emission claims narrow SIMD repair");
        auto batch=narrow;batch.encoded_repair_policy=EncodedRepairPolicy::Batch256;
        const auto control=emit_cpp(residual_source,batch);
        require_codegen(function_codegen(compiled.header,"inline std::uint64_t evaluate_encoded_unchecked(")==
                        function_codegen(control.header,"inline std::uint64_t evaluate_encoded_unchecked("),
                        "repair policy changed the scalar exact path");
        require_codegen(compiled.header==emit_cpp(residual_source,narrow).header,"narrow emission is nondeterministic");
        if(enabled) {
            for(const auto placement:{EncodedCarryPlacement::Eager,EncodedCarryPlacement::Guarded}) {
                auto invalid=narrow;invalid.encoded_carry_placement=placement;
                rejects_codegen([&]{(void)emit_cpp(residual_source,invalid);});
            }
            auto disabled=narrow;disabled.enable_encoded=false;
            rejects_codegen([&]{(void)emit_cpp(residual_source,disabled);});
            rejects_codegen([&]{(void)emit_cpp_portfolio(residual_source,narrow);});
        }
    }
    auto scalar = emit_cpp(plan, {.namespace_name="scalar_kernel",.enable_avx2=false,.lower_predicates=false});
    require_codegen(!scalar.predicate_lowered && !scalar.avx2_emitted, "codegen options ignored");
    require_codegen(scalar.header.find("namespace scalar_kernel") != std::string::npos, "generated namespace missing");
    require_codegen(scalar.header.find("inline std::uint32_t evaluate_simd") == std::string::npos, "disabled SIMD body emitted");
    const auto encoded_scalar = emit_cpp(plan, {.namespace_name="encoded_scalar",.enable_avx2=false});
    require_codegen(encoded_scalar.encoded_emitted && !encoded_scalar.encoded_avx2_emitted, "encoded SIMD option ignored");
    require_codegen(encoded_scalar.header.find("run_encoded_avx2_unchecked") == std::string::npos, "disabled encoded SIMD body emitted");
    const auto difference = parse_program("input x 0 0\ninput y 1 0\nsub d x y\nabs a d\noutput a > 100\n").compile(ranges);
    const auto arithmetic = emit_cpp(difference);
    require_codegen(!arithmetic.predicate_lowered && arithmetic.avx2_emitted, "general arithmetic codegen unavailable");
    require_codegen(!arithmetic.encoded_emitted, "unsupported encoded lowering was exposed");
    require_codegen(arithmetic.header.find("simd_lanes = 4") != std::string::npos, "int64 SIMD width incorrect");
    for (const auto* name : {"", "std", "class", "namespace", "a::b", "_reserved", "a__b", "x;evil", "1bad", "co_await"}) {
        rejects_codegen([&] { (void)emit_cpp(plan, {.namespace_name=name}); });
    }
    rejects_codegen([] { (void)emit_cpp(CompiledProgram{}); });
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_prefix_bits=33});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_strategy=static_cast<EncodedStrategy>(255)});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_schedule=static_cast<EncodedSchedule>(255)});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_carry_placement=static_cast<EncodedCarryPlacement>(255)});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_plane_binding=static_cast<EncodedPlaneBinding>(255)});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_repair_policy=static_cast<EncodedRepairPolicy>(255)});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_repair_policy=EncodedRepairPolicy::Word64});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_strategy=EncodedStrategy::Frontier,
                                         .encoded_repair_policy=EncodedRepairPolicy::Hybrid});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_carry_placement=EncodedCarryPlacement::Guarded});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_strategy=EncodedStrategy::Frontier,
                                         .encoded_carry_placement=EncodedCarryPlacement::Guarded});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_carry_placement=EncodedCarryPlacement::Recompute});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.encoded_strategy=EncodedStrategy::Frontier,
                                         .encoded_carry_placement=EncodedCarryPlacement::Recompute});});
    rejects_codegen([&]{(void)emit_cpp(plan,{.raw_layout=static_cast<RawLayout>(255)});});
    Program extremes;
    const auto min = extremes.constant(std::numeric_limits<i64>::min());
    extremes.output(min, std::numeric_limits<i64>::min());
    const auto extreme_source = emit_cpp(extremes.compile({}));
    require_codegen(extreme_source.header.find("std::numeric_limits<std::int64_t>::min()") != std::string::npos,
                    "int64 minimum lacks a representable generated literal");
}
