#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace suffice {

using i64 = std::int64_t;
using NodeId = std::uint16_t;
inline constexpr std::size_t max_program_nodes = 64;
inline constexpr int max_input_offset = 16;

struct Bounds {
    i64 lo{};
    i64 hi{};
    friend bool operator==(const Bounds&, const Bounds&) = default;
};

enum class Decision : std::uint8_t { False, True, Unknown };
enum class Op : std::uint8_t {
    Load, Constant, Add, Sub, MulConst, Abs, Square, Min, Max
};

struct Load {
    std::uint32_t channel{};
    int offset{};
    Bounds range{};
};

// Operands refer only to earlier entries in CompiledProgram::nodes().
struct Instruction {
    Op op{Op::Constant};
    NodeId a{};
    NodeId b{};
    i64 immediate{};
    std::uint16_t load_index{};
    Bounds range{};
};

class CompiledProgram {
public:
    [[nodiscard]] std::span<const Load> loads() const noexcept;
    [[nodiscard]] std::span<const Instruction> nodes() const noexcept;
    [[nodiscard]] std::size_t node_count() const noexcept;
    [[nodiscard]] NodeId output_node() const noexcept;
    [[nodiscard]] i64 threshold() const noexcept;
    [[nodiscard]] Bounds output_bounds() const noexcept;
    [[nodiscard]] int min_offset() const noexcept;
    [[nodiscard]] int max_offset() const noexcept;

    // Checked interfaces reject missing/out-of-range exact inputs and invalid
    // intervals. Bounds are intersected with the compiled channel ranges.
    [[nodiscard]] i64 evaluate_exact(std::span<const i64> inputs) const;
    [[nodiscard]] Bounds evaluate_bounds(std::span<const Bounds> inputs) const;
    [[nodiscard]] Decision decide(std::span<const Bounds> inputs) const;
    [[nodiscard]] Decision decide(Bounds output_interval) const noexcept;

    // Fast interfaces: caller supplies at least loads().size() entries, each
    // within Load::range. Intervals must be valid subsets of that range.
    // These preconditions and compile-time range checking guarantee no signed
    // arithmetic overflow. No allocation or exception is possible here.
    [[nodiscard]] i64 evaluate_exact_unchecked(std::span<const i64> inputs) const noexcept;
    [[nodiscard]] Bounds evaluate_bounds_unchecked(std::span<const Bounds> inputs) const noexcept;

    [[nodiscard]] std::string describe() const;

private:
    friend class Program;
    std::array<Instruction, max_program_nodes> nodes_{};
    std::array<Load, max_program_nodes> loads_{};
    std::uint16_t node_count_{};
    std::uint16_t load_count_{};
    NodeId output_{};
    i64 threshold_{};
    int min_offset_{};
    int max_offset_{};
};

class Program {
public:
    [[nodiscard]] NodeId load(std::uint32_t channel, int offset = 0);
    [[nodiscard]] NodeId constant(i64 value);
    [[nodiscard]] NodeId add(NodeId a, NodeId b);
    [[nodiscard]] NodeId sub(NodeId a, NodeId b);
    [[nodiscard]] NodeId mul_const(NodeId a, i64 coefficient);
    [[nodiscard]] NodeId abs(NodeId a);
    [[nodiscard]] NodeId square(NodeId a);
    [[nodiscard]] NodeId min(NodeId a, NodeId b);
    [[nodiscard]] NodeId max(NodeId a, NodeId b);
    void output(NodeId value, i64 threshold);
    [[nodiscard]] CompiledProgram compile(std::span<const Bounds> channel_ranges) const;
    [[nodiscard]] std::size_t builder_node_count() const noexcept;

private:
    struct BuilderNode {
        Op op{};
        NodeId a{};
        NodeId b{};
        i64 immediate{};
        std::uint32_t channel{};
        int offset{};
        friend bool operator==(const BuilderNode&, const BuilderNode&) = default;
    };
    [[nodiscard]] NodeId intern(BuilderNode node);
    void check_id(NodeId id) const;
    std::vector<BuilderNode> nodes_;
    NodeId output_{};
    i64 threshold_{};
    bool has_output_{};
};

[[nodiscard]] CompiledProgram compile(const Program& program, std::span<const Bounds> channel_ranges);
[[nodiscard]] Program parse_program(std::string_view text);
[[nodiscard]] Program builtin_program(std::string_view name, i64 threshold,
                                      std::uint32_t channel = 0);
[[nodiscard]] const char* op_name(Op op) noexcept;

} // namespace suffice
