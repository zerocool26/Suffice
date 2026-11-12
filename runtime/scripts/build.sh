#!/usr/bin/env sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
build_dir="$project_dir/build"
configuration=release
native=0
tests=1
run_tests=1

usage() {
    cat <<'HELP'
Usage: scripts/build.sh [--release|--debug|--sanitize] [--native]
                        [--build-dir PATH] [--no-tests|--no-run]

Builds suffice and, by default, builds and runs suffice_tests. Uses CXX when
provided, otherwise c++. No third-party libraries or package downloads.
--native generates a machine-specific binary; omit it for portable artifacts.
--sanitize selects AddressSanitizer and UndefinedBehaviorSanitizer.
HELP
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --release) configuration=release ;;
        --debug) configuration=debug ;;
        --sanitize) configuration=sanitize ;;
        --native) native=1 ;;
        --no-tests) tests=0; run_tests=0 ;;
        --no-run) run_tests=0 ;;
        --build-dir)
            shift
            [ "$#" -gt 0 ] || { printf '%s\n' 'Missing path after --build-dir' >&2; exit 2; }
            build_dir=$1
            ;;
        --help|-h) usage; exit 0 ;;
        *) printf 'Unknown argument: %s\n' "$1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

compiler=${CXX:-c++}
command -v "$compiler" >/dev/null 2>&1 || {
    printf 'C++ compiler not found: %s. Set CXX to a C++20 compiler executable.\n' "$compiler" >&2
    exit 1
}

mkdir -p "$build_dir"
set -- -std=c++20 -pthread -Wall -Wextra -Wpedantic
case "$configuration" in
    release) set -- "$@" -O3 -DNDEBUG ;;
    debug) set -- "$@" -O0 -g ;;
    sanitize) set -- "$@" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer ;;
esac
if [ "$native" -eq 1 ]; then
    set -- "$@" -march=native
fi

printf 'Building Suffice (%s) with %s\n' "$configuration" "$compiler"
"$compiler" "$@" -I"$project_dir/include" \
    "$project_dir/src/ir.cpp" "$project_dir/src/storage.cpp" \
    "$project_dir/src/preimage.cpp" "$project_dir/src/encoded_ranges.cpp" \
    "$project_dir/src/runtime.cpp" "$project_dir/src/vector.cpp" \
    "$project_dir/src/predicate.cpp" "$project_dir/src/adaptive.cpp" \
    "$project_dir/src/wordcode.cpp" "$project_dir/src/frontier.cpp" "$project_dir/src/residual.cpp" "$project_dir/src/boolean_proof.cpp" \
    "$project_dir/src/frontier_runtime.cpp" "$project_dir/src/codegen.cpp" "$project_dir/src/portfolio_codegen.cpp" "$project_dir/src/main.cpp" \
    -o "$build_dir/suffice"

if [ "$tests" -eq 1 ]; then
    "$compiler" "$@" -UNDEBUG -I"$project_dir/include" \
        "$project_dir/src/ir.cpp" "$project_dir/src/storage.cpp" \
        "$project_dir/src/preimage.cpp" "$project_dir/src/encoded_ranges.cpp" \
        "$project_dir/src/runtime.cpp" "$project_dir/src/vector.cpp" \
        "$project_dir/src/predicate.cpp" "$project_dir/src/adaptive.cpp" \
        "$project_dir/src/wordcode.cpp" "$project_dir/src/frontier.cpp" "$project_dir/src/residual.cpp" "$project_dir/src/boolean_proof.cpp" \
        "$project_dir/src/frontier_runtime.cpp" "$project_dir/src/codegen.cpp" "$project_dir/src/portfolio_codegen.cpp" "$project_dir/tests/main.cpp" \
        "$project_dir/tests/test_ir.cpp" "$project_dir/tests/test_storage.cpp" \
        "$project_dir/tests/test_preimage.cpp" "$project_dir/tests/test_encoded_ranges.cpp" \
        "$project_dir/tests/test_runtime.cpp" "$project_dir/tests/test_vector.cpp" \
        "$project_dir/tests/test_predicate.cpp" "$project_dir/tests/test_adaptive.cpp" \
        "$project_dir/tests/test_decode.cpp" "$project_dir/tests/test_ranges.cpp" \
        "$project_dir/tests/test_wordcode.cpp" "$project_dir/tests/test_frontier.cpp" "$project_dir/tests/test_residual.cpp" "$project_dir/tests/test_boolean_proof.cpp" \
        "$project_dir/tests/test_frontier_runtime.cpp" "$project_dir/tests/test_codegen.cpp" "$project_dir/tests/test_portfolio.cpp" -o "$build_dir/suffice_tests"
    if [ "$run_tests" -eq 1 ]; then
        "$build_dir/suffice_tests"
    fi
fi
printf 'Executable: %s/suffice\n' "$build_dir"
