#pragma once
#include "suffice/catalog.hpp"
#include <filesystem>
#include <string>
#include <vector>

namespace suffice::catalog {
struct GenerationLimits {
    payload::Limits snapshot{};
    std::uint64_t max_component_bytes{512ULL<<20};
    std::uint64_t max_metadata_bytes{16ULL<<20};
    std::uint64_t max_path_bytes{4096};
};
struct LoadedGeneration {
    std::string generation;
    std::string manifest_sha256;
    std::shared_ptr<const payload::Snapshot> snapshot;
    std::vector<ArtifactRow> artifacts;
    std::uint64_t component_bytes{};
    std::uint64_t verified_body_bytes{};
};
// Read CURRENT once, then capture one immutable directory. A publisher may
// replace CURRENT concurrently; admitted bytes belong to the captured generation.
// The catalog directory is trusted local storage, not a hostile filesystem.
// Components and metadata are SHA-256 checked, and all feature semantics are
// re-derived from decoded bodies before returning a candidate for publication.
LoadedGeneration load_generation(const std::filesystem::path& catalog_root,
                                 GenerationLimits limits = {});
LoadedGeneration load_generation_id(const std::filesystem::path& catalog_root,
                                    const std::string& generation,
                                    GenerationLimits limits = {});
} // namespace suffice::catalog
