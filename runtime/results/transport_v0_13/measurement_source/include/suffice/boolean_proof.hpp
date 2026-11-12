#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>

namespace suffice {

using BooleanId = std::uint32_t;
enum class BooleanOp : std::uint8_t { And, Or, Xor };
class BooleanProofExhausted : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Small bounded reduced ordered BDD manager. Terminals are 0 and 1. Variable
// order is ascending numeric identifier; no complemented edges or dynamic
// reordering. Canonical equality is valid only inside this one manager.
// This implementation is part of the trusted compiler, not a proof assistant.
class BooleanProof {
public:
    BooleanProof(std::uint32_t node_budget, std::uint64_t step_budget);
    ~BooleanProof();
    BooleanProof(const BooleanProof&) = delete;
    BooleanProof& operator=(const BooleanProof&) = delete;
    BooleanProof(BooleanProof&&) noexcept;
    BooleanProof& operator=(BooleanProof&&) noexcept;

    [[nodiscard]] BooleanId variable(std::uint32_t index);
    [[nodiscard]] BooleanId apply(BooleanOp, BooleanId, BooleanId);
    [[nodiscard]] BooleanId negate(BooleanId);
    [[nodiscard]] bool equivalent_under(BooleanId care, BooleanId left, BooleanId right);
    [[nodiscard]] bool evaluate(BooleanId, std::span<const std::uint8_t> assignment) const;
    [[nodiscard]] std::uint32_t node_count() const noexcept;
    [[nodiscard]] std::uint64_t steps() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace suffice
