#pragma once

#include "suffice/codegen.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace suffice {

struct PortfolioCodegenOptions {
    // Bound the combined artifact. Four variants are fixed, not a user-sized
    // Cartesian search. This is an output limit, not a compiler memory limit.
    std::size_t max_header_bytes{64u * 1024u * 1024u};
};

struct GeneratedPortfolioVariant {
    std::string name;
    std::string namespace_name;
    GeneratedKernel kernel;
};

struct GeneratedPortfolio {
    std::string header;
    std::string description;
    std::array<GeneratedPortfolioVariant, 4> variants;
};

// Emits four exact merged-output implementations and an owning prepared wrapper.
// The wrapper depends on suffice/portfolio.hpp, and validates the payload once.
// Strategy, schedule and carry placement are fixed per variant; other base
// options are retained. Base enable_encoded=false and a non-Batch256 repair
// policy are rejected; narrower repair candidates are not in this portfolio.
[[nodiscard]] GeneratedPortfolio emit_cpp_portfolio(
    const CompiledProgram&, CodegenOptions = {}, PortfolioCodegenOptions = {});

} // namespace suffice
