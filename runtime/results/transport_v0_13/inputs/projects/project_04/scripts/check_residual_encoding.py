#!/usr/bin/env python3
"""Check a finite prefix-state encoding and report a concrete collision witness.

Input JSON supplies case (a known name or full case), state_bits, conditional,
and exactly one of states (one code per prefix) or encoder_hex (packed codes).
This is a bounded research tool, not a production predicate compiler.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import residual_quotient_lab as lab


def check_document(document, limits=None):
    if not isinstance(document, dict):
        raise ValueError("candidate document must be an object")
    if set(document) - {"case", "state_bits", "conditional", "states", "encoder_hex"}:
        raise ValueError("unknown candidate field")
    case = document.get("case")
    if isinstance(case, str):
        case = next((dict(c) for c in lab.CASES if c["name"] == case), None)
        if case is None:
            raise ValueError("unknown case name")
    budget, limits = lab.checked_plan(case, limits=limits)
    bits = document.get("state_bits")
    if type(bits) is not int or not 0 <= bits <= 64:
        raise ValueError("candidate state width must be an integer in 0..64")
    if ("states" in document) == ("encoder_hex" in document):
        raise ValueError("supply exactly one of states or encoder_hex")
    if "states" in document:
        states = document["states"]
    else:
        encoded = document["encoder_hex"]
        if not isinstance(encoded, str):
            raise ValueError("encoder_hex must be a string")
        byte_count = (budget["prefixes"] * bits + 7) // 8
        # Check textual extent before fromhex allocates the decoded bytes.
        if len(encoded) != 2 * byte_count:
            raise ValueError("packed candidate has the wrong length")
        data = bytes.fromhex(encoded)
        if len(data) != byte_count:
            raise ValueError("packed candidate contains whitespace instead of code bytes")
        unused = (budget["prefixes"] * bits) % 8
        if unused and data[-1] >> unused:
            raise ValueError("packed candidate has nonzero padding")
        states = [lab.unpack(data, i, bits) for i in range(budget["prefixes"])]
    return lab.check_encoding(case, states, bits,
                              conditional=document.get("conditional", False), limits=limits)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--max-rows", type=int, default=65536)
    parser.add_argument("--max-evaluations", type=int, default=1_000_000)
    parser.add_argument("--max-signature-bytes", type=int, default=1_000_000)
    args = parser.parse_args()
    limits = {"max_rows_per_case": args.max_rows,
              "max_total_evaluations": args.max_evaluations,
              "max_signature_payload_bytes": args.max_signature_bytes}
    result = {"schema_version": 1, "production_runtime": False,
              "exact_optimum_claimed": False,
              "checker_source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "lab_source_sha256": hashlib.sha256(Path(lab.__file__).read_bytes()).hexdigest()}
    try:
        # Bound file parsing separately from predicate enumeration. This still
        # does not impose a process RSS or Python-object allocation ceiling.
        input_limit = 8 * 1024 * 1024
        if args.input.stat().st_size > input_limit:
            raise lab.BudgetRejected(["candidate input exceeds 8 MiB"])
        # Also cap the actual read if a concurrently changed file grew after stat.
        with args.input.open("rb") as source:
            content = source.read(input_limit + 1)
        if len(content) > input_limit:
            raise lab.BudgetRejected(["candidate input exceeds 8 MiB"])
        result["candidate_sha256"] = hashlib.sha256(content).hexdigest()
        result.update(check_document(json.loads(content), limits))
        exit_code = 0 if result["exact_encoding"] else 1
    except lab.BudgetRejected as error:
        result.update(status="BUDGET_REJECTED", exact_encoding=None,
                      proof_status="UNKNOWN", errors=error.errors)
        exit_code = 2
    except (ValueError, TypeError, KeyError, OSError) as error:
        result.update(status="INVALID_INPUT", exact_encoding=None,
                      proof_status="UNKNOWN", errors=[str(error)])
        exit_code = 2
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"status": result["status"], "output": str(args.output),
                      "exact_encoding": result.get("exact_encoding")}))
    return exit_code


if __name__ == "__main__":
    raise SystemExit(main())
