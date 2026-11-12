#!/usr/bin/env python3
"""Independent Python mask oracle and v0.4 CLI frontier/emission contracts."""

import argparse
import json
from pathlib import Path
import struct
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binary, directory = args.binary.resolve(), args.output.resolve()
    directory.mkdir(parents=True, exist_ok=False)
    commands = []

    def execute(arguments, succeeds=True):
        command = [str(binary), *map(str, arguments)]
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        commands.append({"argv": command, "returncode": result.returncode,
                         "stdout": result.stdout, "stderr": result.stderr})
        if (result.returncode == 0) != succeeds:
            raise AssertionError("Unexpected exit status: " + repr(commands[-1]))

    rows = 1737
    a = [((i * 37) % 181) - 90 for i in range(rows)]
    b = [((i * 19 + 11) % 163) - 81 for i in range(rows)]
    raw, packed = directory / "input.i32", directory / "input.sfp"
    raw.write_bytes(b"".join(struct.pack("<ii", x, y) for x, y in zip(a, b)))
    execute(["pack", "--input", raw, "--output", packed, "--channels", 2, "--block-rows", 32])
    programs = {
        "positive": ("input a 0 0\ninput b 1 0\nmin gate a b\noutput gate > -1\n",
                     [x >= 0 and y >= 0 for x, y in zip(a, b)]),
        "halo": ("input prev 0 -16\ninput next 1 16\nsub delta next prev\nabs magnitude delta\noutput magnitude > 17\n",
                 [abs(b[min(rows - 1, i + 16)] - a[max(0, i - 16)]) > 17 for i in range(rows)]),
    }
    checks = []
    for name, (source, expected) in programs.items():
        program = directory / (name + ".suf")
        program.write_text(source, encoding="utf-8")
        for has_raw, first, count in ((True, 0, rows), (True, 13, 91), (False, 31, 103),
                                     (False, rows - 15, 15), (True, rows, 0)):
            label = f"{name}_{int(has_raw)}_{first}_{count}"
            mask, report = directory / (label + ".mask"), directory / (label + ".json")
            command = ["run", "--input", packed, "--program", program, "--first-row", first,
                       "--row-count", count, "--mask", mask, "--json", report,
                       "--sample-rows", 89, "--sample-windows", 5,
                       "--calibration-work-rows", 1024, "--calibration-repeat", 2,
                       "--calibration-ms", 0]
            if has_raw:
                command += ["--raw", raw]
            execute(command)
            expected_words = [0] * ((count + 63) // 64)
            for local, accepted in enumerate(expected[first:first + count]):
                if accepted:
                    expected_words[local // 64] |= 1 << (local % 64)
            expected_bytes = b"".join(struct.pack("<Q", word) for word in expected_words)
            if mask.read_bytes() != expected_bytes:
                raise AssertionError("Independent range mask mismatch: " + label)
            data = json.loads(report.read_text(encoding="utf-8"))
            if data["stats"]["accepted"] != sum(expected[first:first + count]):
                raise AssertionError("Incorrect accepted count")
            selection = data["selection"]
            if data["schema_version"] != 4 or selection["total_calibration_row_evaluations"] > 1024:
                raise AssertionError("Schema or hard row-work budget failed")
            if selection["reference_is_full"] or not 0 < selection["calibrated_rows"] <= 89:
                raise AssertionError("Sampling did not retain its explicit partial-dataset scope")
            checks.append({"case": label, "accepted": data["stats"]["accepted"],
                           "selected": selection["selected"], "work_rows": selection["total_calibration_row_evaluations"]})
        header, metadata = directory / (name + ".hpp"), directory / (name + "_emission.json")
        execute(["emit-cpp", "--input", packed, "--program", program, "--namespace", "cli_generated",
                 "--output", header, "--json", metadata])
        generated = json.loads(metadata.read_text(encoding="utf-8"))
        if generated["encoded_emitted"] != (name == "positive") or generated["native_compilation_performed"]:
            raise AssertionError("Incorrect code-emission metadata")
        before = header.read_bytes()
        execute(["emit-cpp", "--input", packed, "--program", program, "--output", header], False)
        if header.read_bytes() != before:
            raise AssertionError("Existing generated source was overwritten")
    frontier_checks = []
    program = directory / "positive.suf"
    expected = programs["positive"][1]
    for depth in (0, 1, 12, 31, 32):
        mask = directory / f"forced_frontier_{depth}.mask"
        report = directory / f"forced_frontier_{depth}.json"
        execute(["run", "--input", packed, "--program", program,
                 "--backend", "encoded-frontier", "--frontier-prefix-bits", depth,
                 "--first-row", 13, "--row-count", 1093, "--mask", mask, "--json", report])
        words = [0] * ((1093 + 63) // 64)
        for local, accepted in enumerate(expected[13:13 + 1093]):
            if accepted:
                words[local // 64] |= 1 << (local % 64)
        if mask.read_bytes() != b"".join(struct.pack("<Q", word) for word in words):
            raise AssertionError("Forced frontier differs from independent oracle")
        data = json.loads(report.read_text())
        stats = data["stats"]
        if stats["frontier_prefix_resolved_rows"] + stats["refined_rows"] != 1093:
            raise AssertionError("Frontier work accounting lost rows")
        if depth == 32 and stats["frontier_fallback_words"]:
            raise AssertionError("Full-depth prefix unexpectedly falls back")
        frontier_checks.append({"depth": depth, "stats": stats})
    emission_checks = []
    for depth, schedule, layout in ((0, "legacy", "interleaved"),
                                    (12, "pressure", "planar"),
                                    (32, "pressure", "interleaved")):
        header = directory / f"frontier_{depth}_{schedule}_{layout}.hpp"
        metadata = header.with_suffix(".json")
        execute(["emit-cpp", "--input", packed, "--program", program,
                 "--encoded-strategy", "frontier", "--encoded-prefix-bits", depth,
                 "--encoded-schedule", schedule, "--raw-layout", layout,
                 "--output", header, "--json", metadata])
        data = json.loads(metadata.read_text())
        if (data["schema_version"] != 4 or not data["encoded_frontier_emitted"] or
            data["encoded_prefix_bits"] != depth or data["encoded_schedule"] != schedule or
            data["raw_layout"] != layout or data["native_compilation_performed"]):
            raise AssertionError("Frontier emission metadata mismatch: " + repr(data))
        emission_checks.append(data)
    bad_emission = (["--encoded-strategy", "unknown"], ["--encoded-schedule", "unknown"],
                    ["--raw-layout", "unknown"], ["--encoded-prefix-bits", 33])
    for index, options in enumerate(bad_emission):
        output = directory / f"bad_emit_{index}.hpp"
        execute(["emit-cpp", "--input", packed, "--program", program,
                 "--output", output, *options], False)
        if output.exists():
            raise AssertionError("Invalid emission options created a header")
    invalid_options = [
        ["--first-row", rows + 1, "--row-count", 0],
        ["--first-row", rows - 1, "--row-count", 2],
        ["--sample-windows", 0], ["--calibration", "invalid"],
        ["--max-calibration-mib", 0], ["--calibration-work-rows", 1],
        ["--frontier-prefix-bits", 33],
    ]
    for index, options in enumerate(invalid_options):
        mask = directory / f"invalid_{index}.mask"
        execute(["run", "--input", packed, "--raw", raw, "--program", directory / "positive.suf",
                 "--mask", mask, *options], False)
        if mask.exists():
            raise AssertionError("Invalid request created a completed output")
    result = {"status": "passed", "independent_mask_checks": len(checks),
              "invalid_request_checks": len(invalid_options), "nonoverwrite_checks": 2,
              "emission_metadata_checks": 2 + len(emission_checks),
              "forced_frontier_checks": frontier_checks, "frontier_emissions": emission_checks,
              "invalid_emission_checks": len(bad_emission), "cases": checks, "commands": commands}
    (directory / "result.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(f"PASS {len(checks)} independent complete/sliced masks, sampling budgets, halos, empty ranges, 5 forced frontiers, emission and rejection contracts")


if __name__ == "__main__":
    main()
