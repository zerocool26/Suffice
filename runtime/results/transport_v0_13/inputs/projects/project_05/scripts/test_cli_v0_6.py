#!/usr/bin/env python3
"""Exercise v0.6 proof-budget forwarding, scope metadata and CLI output contracts."""
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
    configurations = [(strategy, placement, [])
                      for strategy in ("residual", "guarded-residual")
                      for placement in ("eager", "guarded")]
    configurations += [("guarded-residual", "guarded", [flag, 0]) for flag in
                       ("--encoded-proof-nodes", "--encoded-proof-steps", "--encoded-proof-attempts")]
    configurations += [("eager", "eager", []), ("frontier", "eager", [])]
    header_fields = {
        "encoded_residual_emitted", "encoded_guarded_residual_emitted", "encoded_guarded_carry_emitted",
        "encoded_residual_exact_all_lanes", "encoded_residual_initial_carries", "encoded_residual_carries",
        "encoded_proof_attempts", "encoded_proof_accepted_rewrites", "encoded_proof_graph_budget_rejections",
        "encoded_proof_bdd_nodes", "encoded_proof_steps", "encoded_proof_exhausted",
        "encoded_residual_scope", "encoded_carry_placement",
    }
    for index, (strategy, placement, extra) in enumerate(configurations):
        header = output / f"kernel_{index}.hpp"
        metadata = header.with_suffix(".json")
        run(["emit-cpp", "--input", packed, "--program", program, "--output", header, "--json", metadata,
             "--encoded-strategy", strategy, "--encoded-carry-placement", placement,
             "--encoded-schedule", "pressure", *extra])
        record = json.loads(metadata.read_text())
        assert record["schema_version"] == 6 and record["version"] == "0.6.0"
        assert record["encoded_strategy"] == strategy and record["encoded_carry_placement"] == placement
        assert record["encoded_guarded_residual_emitted"] == (strategy == "guarded-residual")
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
    assert run_record["schema_version"] == 6 and run_record["version"] == "0.6.0"
    result = {"status": "passed", "metadata_configurations": len(emissions),
              "invalid_emission_requests": len(invalid), "nonoverwrite_checks": 1,
              "independent_python_range_masks": 1, "emissions": emissions, "commands": commands}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k not in {"emissions", "commands"}}, indent=2))


if __name__ == "__main__":
    main()
