#pragma once

// Measurement utility, not part of prepared query execution. Complete Williams
// cycles balance positions and immediate predecessors WITHIN each timing round.
// Randomizing labels and rows reduces fixed-order confounding; it does not erase
// cache state, balance cross-round history, or establish statistical independence.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace suffice {

[[nodiscard]] inline std::size_t balanced_timing_cycle_rows(std::size_t count) {
    if (count == 0 || count > 256) throw std::invalid_argument("timing candidate count must be in 1..256");
    return count == 1 || count % 2 == 0 ? count : count * 2;
}

[[nodiscard]] inline std::vector<std::vector<std::size_t>> balanced_timing_orders(
    std::size_t count, unsigned rounds, std::uint64_t seed) {
    const auto cycle_rows = balanced_timing_cycle_rows(count);
    if (rounds > 1000000) throw std::invalid_argument("too many timing rounds");
    std::vector<std::vector<std::size_t>> result;
    result.reserve(rounds);
    auto random = [&]() noexcept {
        auto value = (seed += 0x9e3779b97f4a7c15ull);
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
        return value ^ (value >> 31);
    };
    auto shuffle = [&](auto& values) {
        for (std::size_t left = values.size(); left > 1; --left)
            std::swap(values[left - 1], values[random() % left]);
    };
    std::vector<std::size_t> base(count), labels(count), row_order(cycle_rows);
    for (std::size_t column = 0; column < count; ++column)
        base[column] = column == 0 ? 0 : column % 2 ? (column + 1) / 2 : count - column / 2;
    while (result.size() < rounds) {
        std::iota(labels.begin(), labels.end(), 0);
        std::iota(row_order.begin(), row_order.end(), 0);
        shuffle(labels);
        shuffle(row_order);
        for (const auto row : row_order) {
            if (result.size() == rounds) break;
            auto& order = result.emplace_back(count);
            const auto shift = row % count;
            const bool reversed = row >= count;
            for (std::size_t column = 0; column < count; ++column) {
                const auto index = reversed ? count - 1 - column : column;
                order[column] = labels[(base[index] + shift) % count];
            }
        }
    }
    return result;
}

} // namespace suffice
