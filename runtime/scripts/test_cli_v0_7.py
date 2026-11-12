#!/usr/bin/env python3
"""Exercise v0.7 scheduling, reconstruction, portfolio metadata and CLI output contracts."""
from pathlib import Path
import argparse
import hashlib
import json
import re
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary, output = args.binary.resolve(), args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    commands = []

    def run(argv, success=True):
        command = [str(binary), *map(str, argv)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        commands.append({"argv": command, "returncode": result.returncode,
                         "stdout": result.stdout, "stderr": result.stderr})
        assert (result.returncode == 0) == success, commands[-1]
        return result

    rows, threshold = 1737, 100000000
    values = [tuple(((r * (15485863 + c * 29) + c * 32452843) % 2000000001) - 1000000000
                    for c in range(4)) for r in range(rows)]
    for r in range(32):
        values[r] = tuple(threshold + ((r >> c) & 1) for c in range(4))
    raw, packed = output / "input.i32", output / "input.sfp"
    raw.write_bytes(b"".join(struct.pack("<iiii", *row) for row in values))
    run(["pack", "--input", raw, "--output", packed, "--channels", 4, "--block-rows", 512])
    program = output / "four_gate.suf"
    program.write_text("input a 0 0\ninput b 1 0\ninput c 2 0\ninput d 3 0\n"
                       "min ab a b\nmin cd c d\nmin result ab cd\noutput result > 100000000\n")
    emissions = []
    configurations = [(strategy, placement, schedule, [])
                      for strategy in ("residual", "guarded-residual")
                      for placement in ("guarded", "recompute")
                      for schedule in ("pressure", "bounds-first")]
    configurations += [("guarded-residual", "recompute", "bounds-first", [flag, 0]) for flag in
                       ("--encoded-proof-nodes", "--encoded-proof-steps", "--encoded-proof-attempts")]
    configurations += [("eager", "eager", "pressure", []), ("frontier", "eager", "bounds-first", [])]
    header_fields = {
        "encoded_residual_emitted", "encoded_guarded_residual_emitted", "encoded_guarded_carry_emitted",
        "encoded_residual_exact_all_lanes", "encoded_residual_initial_carries", "encoded_residual_carries",
        "encoded_proof_attempts", "encoded_proof_accepted_rewrites", "encoded_proof_graph_budget_rejections",
        "encoded_proof_bdd_nodes", "encoded_proof_steps", "encoded_proof_exhausted",
        "encoded_residual_scope", "encoded_carry_placement", "encoded_recompute_carry_emitted",
        "encoded_common_nodes", "encoded_common_planes", "encoded_common_live_values",
        "encoded_cold_prefix_nodes", "encoded_cold_prefix_planes", "encoded_cold_prefix_live_values",
    }
    for index, (strategy, placement, schedule, extra) in enumerate(configurations):
        header = output / f"kernel_{index}.hpp"
        metadata = header.with_suffix(".json")
        run(["emit-cpp", "--input", packed, "--program", program, "--output", header, "--json", metadata,
             "--encoded-strategy", strategy, "--encoded-carry-placement", placement,
             "--encoded-schedule", schedule, *extra])
        record = json.loads(metadata.read_text())
        assert record["schema_version"] == 7 and record["version"] == "0.7.0"
        assert record["encoded_strategy"] == strategy and record["encoded_carry_placement"] == placement
        assert record["encoded_guarded_residual_emitted"] == (strategy == "guarded-residual")
        assert record["encoded_schedule"] == schedule
        assert record["encoded_recompute_carry_emitted"] == (placement == "recompute")
        assert record["encoded_guarded_carry_emitted"] == (placement == "guarded")
        assert record["encoded_residual_emitted"] == (strategy in {"residual", "guarded-residual"})
        assert not record["native_compilation_performed"]
        assert record["encoded_proof_steps"] <= record["encoded_proof_step_budget"]
        assert record["encoded_proof_attempts"] <= record["encoded_proof_attempt_budget"]
        assert record["encoded_proof_bdd_nodes"] <= record["encoded_proof_node_budget"]
        if strategy in {"residual", "guarded-residual"}:
            assert record["encoded_residual_carries"] <= record["encoded_residual_initial_carries"]
        if extra:
            field = {"--encoded-proof-nodes": "encoded_proof_node_budget",
                     "--encoded-proof-steps": "encoded_proof_step_budget",
                     "--encoded-proof-attempts": "encoded_proof_attempt_budget"}[extra[0]]
            assert record[field] == 0 and record["encoded_proof_accepted_rewrites"] == 0
        source = header.read_text()
        for name in header_fields:
            match = re.search(r"\b" + name + r"\s*=\s*([^;]+);", source)
            assert match, name
            literal = match.group(1).strip()
            expected = record[name]
            if isinstance(expected, bool):
                assert literal == str(expected).lower(), name
            elif isinstance(expected, int):
                assert int(literal.rstrip("uUlL")) == expected, name
            else:
                assert json.loads(literal) == expected, name
        emissions.append(record)

    invalid = [
        ["--encoded-carry-placement", "unknown"],
        ["--encoded-schedule", "unknown"],
        ["--encoded-strategy", "eager", "--encoded-carry-placement", "recompute"],
        ["--encoded-strategy", "frontier", "--encoded-carry-placement", "recompute"],
        ["--encoded-strategy", "eager", "--encoded-carry-placement", "guarded"],
        ["--encoded-strategy", "frontier", "--encoded-carry-placement", "guarded"],
        ["--encoded-strategy", "residual", "--encoded-proof-nodes", 0],
        ["--encoded-strategy", "guarded-residual", "--encoded-proof-nodes", 262145],
        ["--encoded-strategy", "guarded-residual", "--encoded-proof-steps", 8000001],
        ["--encoded-strategy", "guarded-residual", "--encoded-proof-attempts", 4097],
        ["--encoded-strategy", "guarded-residual", "--encoded-proof-nodes", -1],
        ["--encoded-strategy", "guarded-residual", "--encoded-proof-steps", "18446744073709551616"],
    ]
    for index, flags in enumerate(invalid):
        target = output / f"invalid_{index}.hpp"
        run(["emit-cpp", "--input", packed, "--program", program, "--output", target, *flags], False)
        assert not target.exists()
    protected = output / "kernel_0.hpp"
    before = hashlib.sha256(protected.read_bytes()).hexdigest()
    run(["emit-cpp", "--input", packed, "--program", program, "--output", protected], False)
    assert hashlib.sha256(protected.read_bytes()).hexdigest() == before

    portfolio_records = []
    for index, flags in enumerate(([], ["--scalar-only"], ["--encoded-proof-steps", 0])):
        target = output / f"portfolio_{index}.hpp"
        report = target.with_suffix(".json")
        run(["emit-portfolio", "--input", packed, "--program", program,
             "--namespace", f"cli_portfolio_{index}", "--output", target, "--json", report, *flags])
        record = json.loads(report.read_text())
        assert record["schema_version"] == 7 and record["version"] == "0.7.0"
        assert record["command"] == "emit-portfolio" and record["portfolio_emitted"]
        assert record["candidate_count"] == 4 and record["fallback_index"] == 0
        assert not record["runtime_calibration_performed"] and not record["native_compilation_performed"]
        assert record["runtime_dependency"] == "suffice/portfolio.hpp"
        expected_names = ("residual_guarded_pressure", "care_guarded_pressure",
                          "residual_recompute_bounds_first", "care_recompute_bounds_first")
        assert [item["name"] for item in record["variants"]] == list(expected_names)
        for candidate, item in enumerate(record["variants"]):
            assert item["index"] == candidate and item["encoded_residual_emitted"]
            assert item["encoded_residual_scope"] == ("all-lanes" if candidate % 2 == 0 else "unresolved-lanes")
            assert item["encoded_carry_placement"] == ("guarded" if candidate < 2 else "recompute")
            assert item["encoded_schedule"] == ("pressure" if candidate < 2 else "bounds-first")
            assert item["encoded_recompute_carry_emitted"] == (candidate >= 2)
            assert item["encoded_guarded_carry_emitted"] == (candidate < 2)
            assert item["encoded_avx2_emitted"] == ("--scalar-only" not in flags)
            if "--encoded-proof-steps" in flags:
                assert item["encoded_proof_accepted_rewrites"] == 0
            assert all(name in item for name in header_fields)
        assert target.stat().st_size == record["header_bytes"]
        assert "PreparedEncodedPortfolio" in target.read_text()
        portfolio_records.append(record)
    invalid_portfolio = [["--encoded-prefix-bits", 33], ["--namespace", "not-a-namespace"],
                         ["--encoded-proof-steps", 8000001], ["--encoded-schedule", "pressure"],
                         ["--original-ir"]]
    for index, flags in enumerate(invalid_portfolio):
        target = output / f"invalid_portfolio_{index}.hpp"
        run(["emit-portfolio", "--input", packed, "--program", program, "--output", target, *flags], False)
        assert not target.exists()
    protected_portfolio = output / "portfolio_0.hpp"
    before_portfolio = hashlib.sha256(protected_portfolio.read_bytes()).hexdigest()
    run(["emit-portfolio", "--input", packed, "--program", program, "--output", protected_portfolio], False)
    assert hashlib.sha256(protected_portfolio.read_bytes()).hexdigest() == before_portfolio

    mask, report = output / "decisions.mask", output / "run.json"
    first, count = 13, 1093
    run(["run", "--input", packed, "--program", program, "--backend", "encoded-frontier",
         "--first-row", first, "--row-count", count, "--mask", mask, "--json", report])
    words = [0] * ((count + 63) // 64)
    for local, row in enumerate(values[first:first + count]):
        if all(value > threshold for value in row):
            words[local // 64] |= 1 << (local % 64)
    assert mask.read_bytes() == b"".join(struct.pack("<Q", word) for word in words)
    run_record = json.loads(report.read_text())
    assert run_record["schema_version"] == 7 and run_record["version"] == "0.7.0"
    result = {"status": "passed", "metadata_configurations": len(emissions),
              "invalid_emission_requests": len(invalid), "nonoverwrite_checks": 2,
              "portfolio_configurations": len(portfolio_records), "invalid_portfolio_requests": len(invalid_portfolio),
              "independent_python_range_masks": 1, "emissions": emissions, "portfolios": portfolio_records, "commands": commands}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k not in {"emissions", "portfolios", "commands"}}, indent=2))


if __name__ == "__main__":
    main()
