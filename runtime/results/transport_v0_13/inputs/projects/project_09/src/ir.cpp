#include "suffice/ir.hpp"

#include <algorithm>
#include <cassert>
#include <charconv>
#include <cctype>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace suffice {
namespace {

constexpr i64 lowest = std::numeric_limits<i64>::min();
constexpr i64 highest = std::numeric_limits<i64>::max();

i64 checked_add(i64 a, i64 b) {
    if ((b > 0 && a > highest - b) || (b < 0 && a < lowest - b))
        throw std::overflow_error("signed 64-bit addition may overflow");
    return a + b;
}

i64 checked_sub(i64 a, i64 b) {
    if ((b > 0 && a < lowest + b) || (b < 0 && a > highest + b))
        throw std::overflow_error("signed 64-bit subtraction may overflow");
    return a - b;
}

i64 checked_mul(i64 a, i64 b) {
    if (a == 0 || b == 0) return 0;
    if (a > 0) {
        if ((b > 0 && a > highest / b) || (b < 0 && b < lowest / a))
            throw std::overflow_error("signed 64-bit multiplication may overflow");
    } else {
        if ((b > 0 && a < lowest / b) || (b < 0 && a < highest / b))
            throw std::overflow_error("signed 64-bit multiplication may overflow");
    }
    return a * b;
}

i64 checked_abs(i64 value) {
    if (value == lowest)
        throw std::overflow_error("absolute value of INT64_MIN is not representable");
    return value < 0 ? -value : value;
}

Bounds checked_bounds(Op op, Bounds a, Bounds b, i64 immediate) {
    switch (op) {
    case Op::Add:
        return {checked_add(a.lo, b.lo), checked_add(a.hi, b.hi)};
    case Op::Sub:
        return {checked_sub(a.lo, b.hi), checked_sub(a.hi, b.lo)};
    case Op::MulConst: {
        const i64 x = checked_mul(a.lo, immediate);
        const i64 y = checked_mul(a.hi, immediate);
        return {std::min(x, y), std::max(x, y)};
    }
    case Op::Abs: {
        const i64 x = checked_abs(a.lo);
        const i64 y = checked_abs(a.hi);
        return {a.lo <= 0 && a.hi >= 0 ? 0 : std::min(x, y), std::max(x, y)};
    }
    case Op::Square: {
        const i64 x = checked_mul(a.lo, a.lo);
        const i64 y = checked_mul(a.hi, a.hi);
        return {a.lo <= 0 && a.hi >= 0 ? 0 : std::min(x, y), std::max(x, y)};
    }
    case Op::Min:
        return {std::min(a.lo, b.lo), std::min(a.hi, b.hi)};
    case Op::Max:
        return {std::max(a.lo, b.lo), std::max(a.hi, b.hi)};
    default:
        throw std::logic_error("invalid arithmetic opcode");
    }
}

bool unary(Op op) {
    return op == Op::MulConst || op == Op::Abs || op == Op::Square;
}

bool binary(Op op) {
    return op == Op::Add || op == Op::Sub || op == Op::Min || op == Op::Max;
}

i64 parse_integer(std::string_view text) {
    if (text.empty()) throw std::invalid_argument("expected a signed integer");
    if (text.front() == '+') {
        text.remove_prefix(1);
        if (text.empty() || text.front() == '-' || text.front() == '+')
            throw std::invalid_argument("invalid signed integer");
    }
    i64 result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
        throw std::invalid_argument("invalid signed integer: " + std::string(text));
    return result;
}

bool valid_name(std::string_view name) {
    if (name.empty()) return false;
    const auto first = static_cast<unsigned char>(name.front());
    if (!(std::isalpha(first) || first == '_')) return false;
    for (char ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (!(std::isalnum(c) || c == '_')) return false;
    }
    return true;
}

} // namespace

const char* op_name(Op op) noexcept {
    switch (op) {
    case Op::Load: return "input";
    case Op::Constant: return "const";
    case Op::Add: return "add";
    case Op::Sub: return "sub";
    case Op::MulConst: return "scale";
    case Op::Abs: return "abs";
    case Op::Square: return "square";
    case Op::Min: return "min";
    case Op::Max: return "max";
    }
    return "invalid";
}

void Program::check_id(NodeId id) const {
    if (id >= nodes_.size()) throw std::invalid_argument("node does not belong to this program");
}

NodeId Program::intern(BuilderNode node) {
    const auto found = std::find(nodes_.begin(), nodes_.end(), node);
    if (found != nodes_.end()) return static_cast<NodeId>(found - nodes_.begin());
    if (nodes_.size() >= 4096)
        throw std::length_error("a source program may contain at most 4096 unique nodes");
    nodes_.push_back(node);
    return static_cast<NodeId>(nodes_.size() - 1);
}

NodeId Program::load(std::uint32_t channel, int offset) {
    if (offset < -max_input_offset || offset > max_input_offset)
        throw std::invalid_argument("input offset must be between -16 and 16");
    BuilderNode node{};
    node.op = Op::Load;
    node.channel = channel;
    node.offset = offset;
    return intern(node);
}

NodeId Program::constant(i64 value) {
    BuilderNode node{};
    node.op = Op::Constant;
    node.immediate = value;
    return intern(node);
}

NodeId Program::add(NodeId a, NodeId b) {
    check_id(a); check_id(b);
    if (nodes_[a].op == Op::Constant && nodes_[b].op == Op::Constant)
        return constant(checked_add(nodes_[a].immediate, nodes_[b].immediate));
    if (nodes_[a].op == Op::Constant && nodes_[a].immediate == 0) return b;
    if (nodes_[b].op == Op::Constant && nodes_[b].immediate == 0) return a;
    if (a > b) std::swap(a, b);
    return intern({Op::Add, a, b, 0, 0, 0});
}

NodeId Program::sub(NodeId a, NodeId b) {
    check_id(a); check_id(b);
    if (a == b) return constant(0);
    if (nodes_[a].op == Op::Constant && nodes_[b].op == Op::Constant)
        return constant(checked_sub(nodes_[a].immediate, nodes_[b].immediate));
    if (nodes_[b].op == Op::Constant && nodes_[b].immediate == 0) return a;
    return intern({Op::Sub, a, b, 0, 0, 0});
}

NodeId Program::mul_const(NodeId a, i64 coefficient) {
    check_id(a);
    if (coefficient == 0) return constant(0);
    if (coefficient == 1) return a;
    if (nodes_[a].op == Op::Constant)
        return constant(checked_mul(nodes_[a].immediate, coefficient));
    return intern({Op::MulConst, a, 0, coefficient, 0, 0});
}

NodeId Program::abs(NodeId a) {
    check_id(a);
    if (nodes_[a].op == Op::Constant) return constant(checked_abs(nodes_[a].immediate));
    if (nodes_[a].op == Op::Abs || nodes_[a].op == Op::Square) return a;
    return intern({Op::Abs, a, 0, 0, 0, 0});
}

NodeId Program::square(NodeId a) {
    check_id(a);
    if (nodes_[a].op == Op::Constant)
        return constant(checked_mul(nodes_[a].immediate, nodes_[a].immediate));
    if (nodes_[a].op == Op::Abs) a = nodes_[a].a;
    return intern({Op::Square, a, 0, 0, 0, 0});
}

NodeId Program::min(NodeId a, NodeId b) {
    check_id(a); check_id(b);
    if (a == b) return a;
    if (nodes_[a].op == Op::Constant && nodes_[b].op == Op::Constant)
        return constant(std::min(nodes_[a].immediate, nodes_[b].immediate));
    if (a > b) std::swap(a, b);
    return intern({Op::Min, a, b, 0, 0, 0});
}

NodeId Program::max(NodeId a, NodeId b) {
    check_id(a); check_id(b);
    if (a == b) return a;
    if (nodes_[a].op == Op::Constant && nodes_[b].op == Op::Constant)
        return constant(std::max(nodes_[a].immediate, nodes_[b].immediate));
    if (a > b) std::swap(a, b);
    return intern({Op::Max, a, b, 0, 0, 0});
}

void Program::output(NodeId value, i64 threshold) {
    check_id(value);
    output_ = value;
    threshold_ = threshold;
    has_output_ = true;
}

std::size_t Program::builder_node_count() const noexcept { return nodes_.size(); }

CompiledProgram Program::compile(std::span<const Bounds> channel_ranges) const {
    if (!has_output_) throw std::invalid_argument("program has no output");
    for (const auto range : channel_ranges) {
        if (range.lo > range.hi || range.lo < std::numeric_limits<std::int32_t>::min() ||
            range.hi > std::numeric_limits<std::int32_t>::max())
            throw std::invalid_argument("channel ranges must be nonempty int32 intervals");
    }
    std::vector<bool> live(nodes_.size(), false);
    live[output_] = true;
    for (std::size_t i = nodes_.size(); i-- > 0;) {
        if (!live[i]) continue;
        const auto& node = nodes_[i];
        if (unary(node.op) || binary(node.op)) live[node.a] = true;
        if (binary(node.op)) live[node.b] = true;
    }
    const auto live_count = static_cast<std::size_t>(std::count(live.begin(), live.end(), true));
    if (live_count > max_program_nodes)
        throw std::length_error("compiled program exceeds the 64-node limit");

    std::vector<NodeId> remap(nodes_.size());
    CompiledProgram result;
    result.threshold_ = threshold_;
    for (std::size_t i = 0; i < nodes_.size(); ++i) {
        if (!live[i]) continue;
        const auto& source = nodes_[i];
        Instruction target{};
        target.op = source.op;
        target.immediate = source.immediate;
        if (unary(source.op) || binary(source.op)) target.a = remap[source.a];
        if (binary(source.op)) target.b = remap[source.b];
        try {
            if (source.op == Op::Load) {
                if (source.channel >= channel_ranges.size())
                    throw std::invalid_argument("input channel does not exist in dataset");
                target.load_index = result.load_count_;
                target.range = channel_ranges[source.channel];
                if (result.load_count_ == 0)
                    result.min_offset_ = result.max_offset_ = source.offset;
                result.loads_[result.load_count_++] = {source.channel, source.offset, target.range};
                result.min_offset_ = std::min(result.min_offset_, source.offset);
                result.max_offset_ = std::max(result.max_offset_, source.offset);
            } else if (source.op == Op::Constant) {
                target.range = {source.immediate, source.immediate};
            } else {
                const Bounds a = result.nodes_[target.a].range;
                const Bounds b = binary(source.op) ? result.nodes_[target.b].range : Bounds{};
                target.range = checked_bounds(source.op, a, b, source.immediate);
            }
        } catch (const std::overflow_error& error) {
            throw std::overflow_error("node " + std::to_string(i) + " (" + op_name(source.op) +
                                      "): " + error.what());
        }
        remap[i] = result.node_count_;
        result.nodes_[result.node_count_++] = target;
    }
    result.output_ = remap[output_];
    return result;
}

CompiledProgram compile(const Program& program, std::span<const Bounds> ranges) {
    return program.compile(ranges);
}

std::span<const Load> CompiledProgram::loads() const noexcept {
    return {loads_.data(), load_count_};
}
std::span<const Instruction> CompiledProgram::nodes() const noexcept {
    return {nodes_.data(), node_count_};
}
std::size_t CompiledProgram::node_count() const noexcept { return node_count_; }
NodeId CompiledProgram::output_node() const noexcept { return output_; }
i64 CompiledProgram::threshold() const noexcept { return threshold_; }
Bounds CompiledProgram::output_bounds() const noexcept { return nodes_[output_].range; }
int CompiledProgram::min_offset() const noexcept { return min_offset_; }
int CompiledProgram::max_offset() const noexcept { return max_offset_; }

i64 CompiledProgram::evaluate_exact_unchecked(std::span<const i64> inputs) const noexcept {
    assert(node_count_ != 0 && inputs.size() >= load_count_);
    std::array<i64, max_program_nodes> values;
    for (std::size_t i = 0; i < node_count_; ++i) {
        const auto& node = nodes_[i];
        switch (node.op) {
        case Op::Load: values[i] = inputs[node.load_index]; break;
        case Op::Constant: values[i] = node.immediate; break;
        case Op::Add: values[i] = values[node.a] + values[node.b]; break;
        case Op::Sub: values[i] = values[node.a] - values[node.b]; break;
        case Op::MulConst: values[i] = values[node.a] * node.immediate; break;
        case Op::Abs: values[i] = values[node.a] < 0 ? -values[node.a] : values[node.a]; break;
        case Op::Square: values[i] = values[node.a] * values[node.a]; break;
        case Op::Min: values[i] = std::min(values[node.a], values[node.b]); break;
        case Op::Max: values[i] = std::max(values[node.a], values[node.b]); break;
        }
    }
    return values[output_];
}

Bounds CompiledProgram::evaluate_bounds_unchecked(std::span<const Bounds> inputs) const noexcept {
    assert(node_count_ != 0 && inputs.size() >= load_count_);
    std::array<Bounds, max_program_nodes> values;
    for (std::size_t i = 0; i < node_count_; ++i) {
        const auto& node = nodes_[i];
        switch (node.op) {
        case Op::Load: values[i] = inputs[node.load_index]; break;
        case Op::Constant: values[i] = {node.immediate, node.immediate}; break;
        case Op::Add:
            values[i] = {values[node.a].lo + values[node.b].lo,
                         values[node.a].hi + values[node.b].hi};
            break;
        case Op::Sub:
            values[i] = {values[node.a].lo - values[node.b].hi,
                         values[node.a].hi - values[node.b].lo};
            break;
        case Op::MulConst: {
            const auto a = values[node.a];
            const i64 x = a.lo * node.immediate;
            const i64 y = a.hi * node.immediate;
            values[i] = {std::min(x, y), std::max(x, y)};
            break;
        }
        case Op::Abs: {
            const auto a = values[node.a];
            const i64 x = a.lo < 0 ? -a.lo : a.lo;
            const i64 y = a.hi < 0 ? -a.hi : a.hi;
            values[i] = {a.lo <= 0 && a.hi >= 0 ? 0 : std::min(x, y), std::max(x, y)};
            break;
        }
        case Op::Square: {
            const auto a = values[node.a];
            const i64 x = a.lo * a.lo;
            const i64 y = a.hi * a.hi;
            values[i] = {a.lo <= 0 && a.hi >= 0 ? 0 : std::min(x, y), std::max(x, y)};
            break;
        }
        case Op::Min:
            values[i] = {std::min(values[node.a].lo, values[node.b].lo),
                         std::min(values[node.a].hi, values[node.b].hi)};
            break;
        case Op::Max:
            values[i] = {std::max(values[node.a].lo, values[node.b].lo),
                         std::max(values[node.a].hi, values[node.b].hi)};
            break;
        }
    }
    return values[output_];
}

i64 CompiledProgram::evaluate_exact(std::span<const i64> inputs) const {
    if (node_count_ == 0) throw std::logic_error("program has not been compiled");
    if (inputs.size() < load_count_) throw std::invalid_argument("too few exact input values");
    for (std::size_t i = 0; i < load_count_; ++i) {
        if (inputs[i] < loads_[i].range.lo || inputs[i] > loads_[i].range.hi)
            throw std::out_of_range("exact input is outside its compiled range");
    }
    return evaluate_exact_unchecked(inputs);
}

Bounds CompiledProgram::evaluate_bounds(std::span<const Bounds> inputs) const {
    if (node_count_ == 0) throw std::logic_error("program has not been compiled");
    if (inputs.size() < load_count_) throw std::invalid_argument("too few input intervals");
    std::array<Bounds, max_program_nodes> narrowed;
    for (std::size_t i = 0; i < load_count_; ++i) {
        if (inputs[i].lo > inputs[i].hi) throw std::invalid_argument("invalid input interval");
        narrowed[i] = {std::max(inputs[i].lo, loads_[i].range.lo),
                       std::min(inputs[i].hi, loads_[i].range.hi)};
        if (narrowed[i].lo > narrowed[i].hi)
            throw std::out_of_range("input interval does not intersect its compiled range");
    }
    return evaluate_bounds_unchecked({narrowed.data(), load_count_});
}

Decision CompiledProgram::decide(std::span<const Bounds> inputs) const {
    return decide(evaluate_bounds(inputs));
}

Decision CompiledProgram::decide(Bounds interval) const noexcept {
    if (interval.lo > interval.hi) return Decision::Unknown;
    if (interval.lo > threshold_) return Decision::True;
    if (interval.hi <= threshold_) return Decision::False;
    return Decision::Unknown;
}

std::string CompiledProgram::describe() const {
    std::ostringstream stream;
    stream << "nodes=" << node_count_ << " loads=" << load_count_
           << " offsets=[" << min_offset_ << ',' << max_offset_ << "]\n";
    for (std::size_t i = 0; i < node_count_; ++i) {
        const auto& node = nodes_[i];
        stream << op_name(node.op) << " n" << i;
        if (node.op == Op::Load) {
            const auto& load = loads_[node.load_index];
            stream << ' ' << load.channel << ' ' << load.offset;
        } else if (node.op == Op::Constant) {
            stream << ' ' << node.immediate;
        } else {
            stream << " n" << node.a;
            if (binary(node.op)) stream << " n" << node.b;
            if (node.op == Op::MulConst) stream << ' ' << node.immediate;
        }
        stream << " # [" << node.range.lo << ',' << node.range.hi << "]\n";
    }
    stream << "output n" << output_ << " > " << threshold_ << '\n';
    return stream.str();
}

Program parse_program(std::string_view text) {
    Program program;
    std::unordered_map<std::string, NodeId> names;
    bool has_output = false;
    std::size_t line_number = 0;
    std::istringstream lines{std::string(text)};
    std::string line;
    while (std::getline(lines, line)) {
        ++line_number;
        const auto comment = line.find('#');
        if (comment != std::string::npos) line.erase(comment);
        std::istringstream statements(line);
        std::string statement;
        while (std::getline(statements, statement, ';')) {
            std::istringstream words(statement);
            std::vector<std::string> tokens;
            for (std::string token; words >> token;) tokens.push_back(std::move(token));
            if (tokens.empty()) continue;
            try {
                auto count = [&](std::size_t n) {
                    if (tokens.size() != n)
                        throw std::invalid_argument("wrong argument count for " + tokens[0]);
                };
                auto lookup = [&](std::size_t n) {
                    const auto found = names.find(tokens.at(n));
                    if (found == names.end())
                        throw std::invalid_argument("undefined value: " + tokens.at(n));
                    return found->second;
                };
                const auto& op = tokens[0];
                if (op == "output") {
                    count(4);
                    if (has_output) throw std::invalid_argument("only one output is permitted");
                    if (tokens[2] != ">") throw std::invalid_argument("output comparison must be >");
                    program.output(lookup(1), parse_integer(tokens[3]));
                    has_output = true;
                    continue;
                }
                if (tokens.size() < 2 || !valid_name(tokens[1]))
                    throw std::invalid_argument("expected a valid value name");
                if (names.contains(tokens[1]))
                    throw std::invalid_argument("duplicate value name: " + tokens[1]);
                NodeId id{};
                if (op == "input") {
                    count(4);
                    const i64 channel = parse_integer(tokens[2]);
                    const i64 offset = parse_integer(tokens[3]);
                    if (channel < 0 || channel > std::numeric_limits<std::uint32_t>::max())
                        throw std::invalid_argument("input channel must be a uint32 index");
                    if (offset < -max_input_offset || offset > max_input_offset)
                        throw std::invalid_argument("input offset must be between -16 and 16");
                    id = program.load(static_cast<std::uint32_t>(channel), static_cast<int>(offset));
                } else if (op == "const") {
                    count(3); id = program.constant(parse_integer(tokens[2]));
                } else if (op == "add" || op == "sub" || op == "min" || op == "max") {
                    count(4);
                    const auto a = lookup(2), b = lookup(3);
                    if (op == "add") id = program.add(a, b);
                    else if (op == "sub") id = program.sub(a, b);
                    else if (op == "min") id = program.min(a, b);
                    else id = program.max(a, b);
                } else if (op == "scale" || op == "mul_const") {
                    count(4); id = program.mul_const(lookup(2), parse_integer(tokens[3]));
                } else if (op == "abs" || op == "square") {
                    count(3);
                    id = op == "abs" ? program.abs(lookup(2)) : program.square(lookup(2));
                } else {
                    throw std::invalid_argument("unknown instruction: " + op);
                }
                names.emplace(tokens[1], id);
            } catch (const std::exception& error) {
                throw std::invalid_argument("line " + std::to_string(line_number) + ": " + error.what());
            }
        }
    }
    if (!has_output) throw std::invalid_argument("program has no output");
    return program;
}

Program builtin_program(std::string_view name, i64 threshold, std::uint32_t channel) {
    Program program;
    const auto current = program.load(channel, 0);
    NodeId result = current;
    if (name == "identity") {
        result = current;
    } else if (name == "delta") {
        const auto previous = program.load(channel, -1);
        result = program.abs(program.sub(current, previous));
    } else if (name == "fir3") {
        const auto previous = program.load(channel, -1);
        const auto next = program.load(channel, 1);
        result = program.abs(program.add(program.sub(previous, program.mul_const(current, 2)), next));
    } else if (name == "energy") {
        result = program.square(current);
    } else {
        throw std::invalid_argument("unknown builtin program: " + std::string(name));
    }
    program.output(result, threshold);
    return program;
}

} // namespace suffice
