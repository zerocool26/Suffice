# Integer program representation

Suffice's first compiler accepts a small, pure integer program. Its input values
are signed 32-bit samples; its intermediates are signed 64-bit mathematical
integers. The compiler rejects a program when its declared input ranges do not
establish that every reachable operation fits in `int64_t`.

The output is a single strict predicate: `score > threshold`. Equality is false.
This contract permits the execution engine to finish a decision using an interval
that lies entirely above or below the threshold. Ambiguous intervals request more
input detail. A point interval produces the same decision as exact evaluation.

The current implementation is an interpreted, compact instruction DAG with
compile-time simplification and range certification. It does not generate machine
code, prove arbitrary programs, or infer a floating-point model's quality.

## Language

One statement is written per line. Semicolons can separate statements on a line;
`#` starts a comment that extends to the end of the line. Names must be unique
and defined before they are used.

```text
# Absolute three-sample second difference.
input center 0 0
input left 0 -1
input right 0 1
scale twice center 2
sub first left twice
add second first right
abs magnitude second
output magnitude > 100000
```

Input statements specify a channel index and an offset relative to the output
sample. Offsets are limited to `[-16, 16]`. The execution engine applies its
documented boundary rule before supplying values to the IR; the evaluator itself
does not index or clamp a dataset.

| Statement | Meaning |
| --- | --- |
| `input name channel offset` | Load an int32 sample as an int64 value |
| `const name value` | An int64 constant |
| `add name left right` | Integer addition |
| `sub name left right` | Integer subtraction |
| `scale name input coefficient` | Multiply by an int64 constant |
| `abs name input` | Absolute value |
| `square name input` | Square |
| `min name left right` | Minimum |
| `max name left right` | Maximum |
| `output name > threshold` | Strict threshold decision |

`mul_const` is an alias for `scale`. Exactly one output statement is required.
The parser reports the source line for invalid statements. Integer literals are
decimal, may have a sign, and must fit in int64. The language has no loops,
mutation, dynamic dispatch, implicit casts, or external effects.

## Builtin programs

`builtin_program(name, threshold, channel)` supplies the following graphs:

| Name | Score |
| --- | --- |
| `identity` | `x[i]` |
| `delta` | `abs(x[i] - x[i-1])` |
| `fir3` | `abs(x[i-1] - 2*x[i] + x[i+1])` |
| `energy` | `x[i] * x[i]` |

The threshold is a caller-supplied value. All four builtins can be certified over
the full int32 input range. Squaring a *derived* expression with a wider range,
such as the three-sample residual, may overflow and is rejected unless the actual
channel ranges prove it safe.

## Compiler behavior

The builder interns identical nodes, including identical `(channel, offset)`
loads. Addition, minimum, and maximum have canonical operand order for common
subexpression elimination. Constant arithmetic is folded with overflow checks.
Implemented identities include:

- `x + 0`, `x - 0`, and `x * 1` become `x`.
- `x - x` and `x * 0` become zero.
- Repeated absolute value is removed.
- `square(abs(x))` becomes `square(x)`.
- `abs(square(x))` becomes `square(x)`.
- `min(x,x)` and `max(x,x)` become `x`.

Compilation walks backward from the output, removes unreachable nodes, remaps
the remaining instructions, and propagates integer intervals. A source builder
can hold up to 4096 unique nodes; no more than 64 reachable instructions may
remain after pruning. Reachable input channels must exist in the supplied
dataset ranges. Each declared channel range must be a nonempty int32 interval.

Range checking uses portable comparisons and division before arithmetic; it
does not depend on `__int128`, compiler overflow extensions, or floating point.
The compiler rejects any possible overflow under its interval analysis. This is
conservative: a safe expression can be rejected when interval arithmetic loses
correlations that the current simplifier cannot recover.

The range certificate assumes that actual input samples respect the declared
channel ranges. A caller using the unchecked evaluator is responsible for that
assumption, including the integrity of its input format and metadata.

## C++ interface

```cpp
#include "suffice/ir.hpp"
#include <array>

using namespace suffice;

auto source = builtin_program("delta", 100);
std::array<Bounds, 1> ranges{{{-1000, 1000}}};
auto plan = source.compile(ranges);

// Inputs follow plan.loads() order, not channel order.
std::array<i64, 2> exact{{200, 50}};
auto score = plan.evaluate_exact(exact); // 150

std::array<Bounds, 2> coarse{{{190, 210}, {40, 60}}};
auto interval = plan.evaluate_bounds(coarse); // [130, 170]
auto decision = plan.decide(interval); // Decision::True
```

`loads()` provides stable source-order entries with `channel`, `offset`, and
`range`. Common identical loads appear once. `nodes()` exposes the optimized
instructions and their certified ranges. `output_node()`, `threshold()`,
`output_bounds()`, `min_offset()`, and `max_offset()` describe the plan.
`describe()` produces a human-readable listing of the compiled graph and ranges;
its first summary line is diagnostic text, not an input-language statement.

The checked exact evaluator rejects values outside their declared range. The
checked interval evaluator intersects coarse intervals with each load's declared
range and rejects invalid or disjoint intervals. Both reject missing inputs or
an uninitialized plan. Extra entries in an input span are ignored.

For the steady-state execution loop, the API also provides
`evaluate_exact_unchecked()` and `evaluate_bounds_unchecked()`. These are
allocation-free and `noexcept`. The caller must supply a compiled plan, enough
inputs in `loads()` order, and values or valid intervals entirely within each
load's certified range. Violating these preconditions can cause invalid results
or undefined behavior. The checked APIs are the appropriate boundary for callers
that have not already established those conditions.

All evaluations use fixed 64-entry stack scratch arrays. The builder, parser,
compilation process, and diagnostic listing may allocate. They are setup work,
separate from per-sample evaluation.

## Interval semantics

For a supported operation, the evaluator computes an enclosure of all possible
results consistent with its input intervals. For example:

- Addition maps `[a,b] + [c,d]` to `[a+c,b+d]`.
- Subtraction maps `[a,b] - [c,d]` to `[a-d,b-c]`.
- Absolute value and square produce a lower bound of zero when the input
  interval contains zero.
- Negative scaling reverses interval endpoint order.

For an output enclosure `[L,U]` and threshold `t`:

- `L > t` establishes `Decision::True`.
- `U <= t` establishes `Decision::False`.
- Otherwise the result is `Decision::Unknown`.

These are deterministic integer bounds, not probabilistic confidence intervals.
An unknown result is not an error and is not a guessed answer: it tells the
execution engine to obtain more information.

## Verification

`tests/test_ir.cpp` exports the global `void test_ir()` entry for the integrated
test runner. It checks exact arithmetic exhaustively on small integer domains,
checks every point in thousands of interval boxes, and runs 10,000 deterministic
random interval cases. It also covers strict threshold ties, optimizer identities,
dead-node pruning, the instruction limit, parser errors, channel contracts,
INT64 boundary operations, and rejection of potentially overflowing expressions.

The standalone test module was built with GCC 14.2 using C++20, warnings as
errors, and optimization. Those checks do not substitute for a Windows compiler
run or for validation of the separate storage and execution components.
