#!/usr/bin/env python3
"""Record checked lab results, concrete encoding witnesses and API regressions."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import re
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=ROOT / "results/residual_quotient_v0_9")
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    commands = []
    def run(argv, label, expected=0):
        proc = subprocess.run([str(x) for x in argv], cwd=ROOT, capture_output=True, text=True)
        log = output / f"{label}.log"
        log.write_text(proc.stdout + proc.stderr)
        commands.append({"argv": [str(x) for x in argv], "returncode": proc.returncode,
                         "expected_returncode": expected, "log": str(log.relative_to(output)),
                         "log_sha256": digest(log)})
        if proc.returncode != expected:
            raise RuntimeError(f"{label} failed: see {log}")
    run([sys.executable, ROOT / "scripts/residual_quotient_lab.py", "--output", output / "result.json"], "exhaustive_lab")
    run([sys.executable, "-m", "unittest", "discover", "-s", "tests", "-p", "test_residual_quotient_v0_9.py", "-v"], "api_and_witness_tests")
    test_counts = re.findall(r"\bRan (\d+) tests?\b", (output / "api_and_witness_tests.log").read_text())
    if len(test_counts) != 1:
        raise RuntimeError("cannot establish the number of executed API tests")
    api_test_count = int(test_counts[0])
    expected = {"magnitude_full_8bit": 0, "magnitude_alias_7bit": 1,
                "magnitude_conditional_alias_7bit": 1,
                "sum_conditional_0bit": 0, "sum_full_alias_0bit": 1}
    checks = []
    for name, exit_code in expected.items():
        candidate = ROOT / "examples/continuation" / f"{name}.json"
        target = output / f"{name}.json"
        run([sys.executable, ROOT / "scripts/check_residual_encoding.py", "--input", candidate,
             "--output", target], name, exit_code)
        result = json.loads(target.read_text())
        checks.append({"name": name, "status": result["status"],
                       "exact_encoding": result["exact_encoding"],
                       "predicate_evaluations": result["predicate_evaluations"],
                       "witness": result["witness"], "result_sha256": digest(target),
                       "candidate_sha256": digest(candidate)})
    rejected = output / "budget_rejected.json"
    run([sys.executable, ROOT / "scripts/check_residual_encoding.py", "--input",
         ROOT / "examples/continuation/sum_conditional_0bit.json", "--output", rejected,
         "--max-rows", 0], "budget_rejected", 2)
    evidence = json.loads((output / "result.json").read_text())
    unknown = json.loads(rejected.read_text())
    if unknown["proof_status"] != "UNKNOWN" or unknown["exact_encoding"] is not None:
        raise RuntimeError("budget rejection was incorrectly presented as a proof")
    files = [ROOT / "scripts/residual_quotient_lab.py", ROOT / "scripts/check_residual_encoding.py",
             Path(__file__), ROOT / "tests/test_residual_quotient_v0_9.py"]
    result = {"schema_version": 1, "status": "PASS",
              "created_utc": datetime.now(timezone.utc).isoformat(),
              "python": sys.version, "source_hashes": {str(p.relative_to(ROOT)): digest(p) for p in files},
              "commands": commands, "exhaustive_cases": len(evidence["cases"]),
              "verified_rows_per_decoder": sum(c["verified_rows_per_decoder"] for c in evidence["cases"]),
              "exhaustive_predicate_evaluations": evidence["preflight_totals"]["evaluations"],
              "candidate_checks": checks,
              "candidate_predicate_evaluations": sum(c["predicate_evaluations"] for c in checks),
              "api_test_count": api_test_count,
              "budget_rejection": {"status": unknown["status"], "proof_status": unknown["proof_status"],
                                   "result_sha256": digest(rejected)},
              "scope": "Finite common-suffix-domain encodings; no runtime speed, process RSS, symbolic proof, or candidate decoder implementation claim."}
    (output / "validation.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
    print(json.dumps({"status": "PASS", "output": str(output), "candidate_checks": len(checks),
                      "verified_rows_per_decoder": result["verified_rows_per_decoder"]}))


if __name__ == "__main__":
    main()
