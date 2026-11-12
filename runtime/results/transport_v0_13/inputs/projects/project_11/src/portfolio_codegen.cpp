#include "suffice/portfolio_codegen.hpp"

#include <array>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace suffice {
namespace {

// The outer namespace must itself be valid, not merely its suffixed variant
// names. Keep the same deliberately narrow identifier contract as emit_cpp.
bool valid_portfolio_namespace(std::string_view name) {
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    if (name.empty() || !letter(name.front()) || name.find("__") != std::string_view::npos) return false;
    for (const char c : name) if (!letter(c) && !(c >= '0' && c <= '9') && c != '_') return false;
    constexpr std::array keywords{
        "alignas", "alignof", "and", "and_eq", "asm", "auto", "bitand", "bitor", "bool",
        "break", "case", "catch", "char", "char8_t", "char16_t", "char32_t", "class",
        "compl", "concept", "const", "consteval", "constexpr", "constinit", "const_cast",
        "continue", "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "double", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
        "false", "float", "for", "friend", "goto", "if", "inline", "int", "long",
        "mutable", "namespace", "new", "noexcept", "not", "not_eq", "nullptr", "operator",
        "or", "or_eq", "private", "protected", "public", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "template", "this", "thread_local", "throw",
        "true", "try", "typedef", "typeid", "typename", "union", "unsigned", "using",
        "virtual", "void", "volatile", "wchar_t", "while", "xor", "xor_eq", "std"
    };
    for (auto keyword : keywords) if (name == keyword) return false;
    return true;
}

} // namespace

GeneratedPortfolio emit_cpp_portfolio(const CompiledProgram& program,
                                      CodegenOptions base,
                                      PortfolioCodegenOptions limits) {
    if (!valid_portfolio_namespace(base.namespace_name))
        throw std::invalid_argument("invalid or reserved generated portfolio namespace");
    if (!base.enable_encoded)
        throw std::invalid_argument("generated portfolio requires encoded execution");
    if (base.encoded_repair_policy != EncodedRepairPolicy::Batch256)
        throw std::invalid_argument("fixed generated portfolio requires batch256 repair policy");
    if (!limits.max_header_bytes)
        throw std::invalid_argument("generated portfolio header budget must be positive");
    constexpr std::array names{
        "residual_guarded_pressure", "care_guarded_pressure",
        "residual_recompute_bounds_first", "care_recompute_bounds_first"
    };
    GeneratedPortfolio result;
    std::size_t variant_bytes = 0;
    for (unsigned index = 0; index < result.variants.size(); ++index) {
        auto options = base;
        // Appending a letter avoids accidentally creating a reserved double
        // underscore when a valid caller namespace ends with an underscore.
        options.namespace_name += "PortfolioV" + std::to_string(index);
        options.encoded_strategy = index % 2 ? EncodedStrategy::GuardedResidual : EncodedStrategy::Residual;
        options.encoded_schedule = index < 2 ? EncodedSchedule::Pressure : EncodedSchedule::BoundsFirst;
        options.encoded_carry_placement = index < 2 ? EncodedCarryPlacement::Guarded : EncodedCarryPlacement::Recompute;
        auto& variant = result.variants[index];
        variant.name = names[index];
        variant.namespace_name = options.namespace_name;
        variant.kernel = emit_cpp(program, options);
        if (!variant.kernel.encoded_emitted || !variant.kernel.encoded_residual_emitted)
            throw std::invalid_argument("generated portfolio requires complete encoded residual lowering");
        if (variant.kernel.header.size() > limits.max_header_bytes - variant_bytes)
            throw std::length_error("generated portfolio exceeds combined header byte budget");
        variant_bytes += variant.kernel.header.size();
    }
    std::ostringstream out;
    out << "#pragma once\n// Fixed exact-kernel portfolio. Requires the Suffice portfolio runtime header.\n"
           "#include \"suffice/portfolio.hpp\"\n\n";
    for (const auto& variant : result.variants) out << variant.kernel.header << '\n';
    out << "namespace " << base.namespace_name << " {\n"
           "inline constexpr std::size_t portfolio_candidate_count = 4;\n"
           "inline constexpr std::array<std::string_view,4> portfolio_candidate_names{\n";
    for (const auto name : names) out << "    \"" << name << "\",\n";
    out << "};\n\n// Immutable Dataset storage may be shared; the prepared owner and its children\n"
           "// cache mutable views and are not reentrant or safe for concurrent calls, even\n"
           "// through a const owner. Use one prepared instance per worker or external locking.\n"
           "template<class Dataset> class PreparedEncodedPortfolio {\n";
    for (unsigned index = 0; index < result.variants.size(); ++index)
        out << "    using K" << index << " = ::" << result.variants[index].namespace_name
            << "::PreparedEncodedKernel<Dataset>;\n";
    out << R"CPP(    template<class Kernel>
    static std::uint64_t invoke(void* context, std::uint64_t first, std::uint64_t count,
                                std::span<std::uint64_t> mask) {
        return static_cast<Kernel*>(context)->run_range(first,count,mask);
    }
public:
    explicit PreparedEncodedPortfolio(const Dataset& data, bool inputs_already_verified=false,
                                       bool allow_simd=true)
        : k0_(data,inputs_already_verified,allow_simd), k1_(data,true,allow_simd),
          k2_(data,true,allow_simd), k3_(data,true,allow_simd),
          selector_(k0_.rows(),std::array<::suffice::PortfolioCandidate,4>{{
              {portfolio_candidate_names[0],&k0_,&invoke<K0>,k0_.uses_simd()},
              {portfolio_candidate_names[1],&k1_,&invoke<K1>,k1_.uses_simd()},
              {portfolio_candidate_names[2],&k2_,&invoke<K2>,k2_.uses_simd()},
              {portfolio_candidate_names[3],&k3_,&invoke<K3>,k3_.uses_simd()}
          }}) {}
    PreparedEncodedPortfolio(const PreparedEncodedPortfolio&)=delete;
    PreparedEncodedPortfolio& operator=(const PreparedEncodedPortfolio&)=delete;
    PreparedEncodedPortfolio(PreparedEncodedPortfolio&&)=delete;
    PreparedEncodedPortfolio& operator=(PreparedEncodedPortfolio&&)=delete;
    [[nodiscard]] std::uint64_t rows() const noexcept { return selector_.rows(); }
    [[nodiscard]] std::size_t candidate_count() const noexcept { return selector_.candidate_count(); }
    [[nodiscard]] std::size_t selected_index() const noexcept { return selector_.selected_index(); }
    [[nodiscard]] std::string_view selected_name() const noexcept { return selector_.selected_name(); }
    [[nodiscard]] std::string_view candidate_name(std::size_t index) const { return selector_.candidate(index).name; }
    [[nodiscard]] bool uses_simd() const noexcept { return selector_.uses_simd(); }
    [[nodiscard]] std::size_t view_storage_bytes() const noexcept {
        return k0_.view_storage_bytes()+k1_.view_storage_bytes()+k2_.view_storage_bytes()+k3_.view_storage_bytes();
    }
    std::uint64_t run(std::span<std::uint64_t> mask) const { return selector_.run(mask); }
    std::uint64_t run_range(std::uint64_t first, std::uint64_t count,
                            std::span<std::uint64_t> mask) const { return selector_.run_range(first,count,mask); }
    [[nodiscard]] ::suffice::PortfolioCalibrationReport calibrate(
        std::uint64_t first, std::uint64_t count,
        std::span<const std::uint64_t> expected, std::span<std::uint64_t> scratch,
        const ::suffice::PortfolioCalibrationOptions& options) {
        return selector_.calibrate(first,count,expected,scratch,options);
    }
private:
    // Execution updates each child's cached plane views. These caches remain
    // legally writable even when this owning wrapper itself is const.
    mutable K0 k0_;
    mutable K1 k1_;
    mutable K2 k2_;
    mutable K3 k3_;
    ::suffice::BoundedPortfolio selector_;
};
)CPP";
    out << "} // namespace " << base.namespace_name << '\n';
    result.header = std::move(out).str();
    if (result.header.size() > limits.max_header_bytes)
        throw std::length_error("generated portfolio exceeds combined header byte budget");
    result.description = "Four exact merged-output encoded kernels; fallback is all-lane residual/guarded/pressure. "
        "Bounded calibration requires an external exact mask, separate scratch, explicit work/time/reuse limits, "
        "and complete balanced timing cycles. Calibration observations are not held-out performance evidence.";
    return result;
}

} // namespace suffice
