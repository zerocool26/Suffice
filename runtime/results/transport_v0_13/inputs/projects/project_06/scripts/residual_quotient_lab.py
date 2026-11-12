#!/usr/bin/env python3
"""Bounded exact residual-quotient experiment; independent of production code."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from collections.abc import Sequence


DEFAULT_LIMITS = {"max_rows_per_case": 65536,
                  "max_total_evaluations": 1_000_000,
                  "max_signature_payload_bytes": 1_000_000}


class BudgetRejected(ValueError):
    """No enumeration took place; rejection is not an optimality result."""

    def __init__(self, errors):
        self.errors = list(errors)
        super().__init__("; ".join(self.errors))


def checked_limits(limits=None):
    result = dict(DEFAULT_LIMITS)
    if limits is not None:
        if not isinstance(limits, dict) or set(limits) - set(result):
            raise ValueError("unknown analysis budget")
        result.update(limits)
    if any(type(value) is not int or value < 0 for value in result.values()):
        raise ValueError("budgets must be nonnegative integers")
    return result


def budget_errors(plans, limits):
    errors = []
    if any(p["rows"] > limits["max_rows_per_case"] for p in plans):
        errors.append("row budget exceeded")
    if sum(p["predicate_evaluations"] for p in plans) > limits["max_total_evaluations"]:
        errors.append("evaluation budget exceeded")
    if sum(p["signature_payload_upper_bound_bytes"] for p in plans) > limits["max_signature_payload_bytes"]:
        errors.append("signature payload budget exceeded")
    return errors


def checked_plan(case, cached_plan=None, limits=None):
    """Validate the authoritative case, optional cached dimensions and limits.

    Public Python entry points call this before allocation or predicate work.
    A caller cannot increase dimensions by forging the old `budget` argument.
    These are logical-work/payload bounds, not process-memory isolation.
    """
    authoritative = plan(case)
    if cached_plan is not None and cached_plan != authoritative:
        raise ValueError("cached analysis plan disagrees with authoritative case")
    limits = checked_limits(limits)
    errors = budget_errors([authoritative], limits)
    if errors:
        raise BudgetRejected(errors)
    return authoritative, limits


# Signed two's-complement channels; p high bits and w-p low bits per channel.
CASES = [
    {"name": f"magnitude_two_p{p}", "channels": 2, "width": 6, "prefix_bits": p,
     "predicate": "sum_squares", "threshold": 225} for p in (2, 3, 4)
] + [
    {"name": "signed_sum_two_p3", "channels": 2, "width": 6, "prefix_bits": 3,
     "predicate": "sum", "threshold": 7},
] + [
    {"name": f"positive_and_four_p{p}", "channels": 4, "width": 4, "prefix_bits": p,
     "predicate": "all_greater", "threshold": 0} for p in (1, 2, 3)
] + [
    {"name": "magnitude_four_p2", "channels": 4, "width": 4, "prefix_bits": 2,
     "predicate": "sum_squares", "threshold": 64},
    {"name": "constant_true_p2", "channels": 2, "width": 4, "prefix_bits": 2,
     "predicate": "sum_squares", "threshold": -1},
]


def width_for(count: int) -> int:
    """Zero or one available state needs no retained state bits."""
    return (count - 1).bit_length() if count > 1 else 0


def pack(values, width: int) -> bytes:
    values = list(values)
    out = bytearray((len(values) * width + 7) // 8)
    for i, value in enumerate(values):
        if value < 0 or (width and value >= 1 << width) or (not width and value):
            raise ValueError("state does not fit packed width")
        for bit in range(width):
            out[(i * width + bit) // 8] |= ((value >> bit) & 1) << ((i * width + bit) % 8)
    return bytes(out)


def unpack(data: bytes, index: int, width: int) -> int:
    return sum(((data[(index * width + b) // 8] >> ((index * width + b) % 8)) & 1) << b
               for b in range(width))


def fields(code: int, bits: int, channels: int) -> list[int]:
    return [(code >> (channel * bits)) & ((1 << bits) - 1) for channel in range(channels)]


def inputs(case, prefix: int, suffix: int) -> list[int]:
    c, w, p = case["channels"], case["width"], case["prefix_bits"]
    high, low = fields(prefix, p, c), fields(suffix, w - p, c)
    raw = [(hi << (w - p)) | lo for hi, lo in zip(high, low)]
    return [x - (1 << w) if x & (1 << (w - 1)) else x for x in raw]


def predicate(case, values) -> bool:
    kind, t = case["predicate"], case["threshold"]
    if kind == "sum_squares":
        return sum(x * x for x in values) > t
    if kind == "sum":
        return sum(values) > t
    if kind == "all_greater":
        return all(x > t for x in values)
    raise ValueError("unknown predicate")


def plan(case):
    if not isinstance(case, dict):
        raise ValueError("case must be a dictionary")
    for key in ("channels", "width", "prefix_bits", "threshold"):
        if type(case.get(key)) is not int:
            raise ValueError(f"{key} must be an integer")
    if not isinstance(case.get("name"), str) or not case["name"]:
        raise ValueError("case requires a nonempty name")
    if case.get("predicate") not in ("sum_squares", "sum", "all_greater"):
        raise ValueError("unknown predicate")
    c, w, p = case["channels"], case["width"], case["prefix_bits"]
    # Reject unreasonable dimensions before shifts or dimension-driven allocation.
    if not (1 <= c <= 4 and 2 <= w <= 8 and 0 <= p <= w and c * w <= 20):
        raise ValueError("dimension hard limit exceeded")
    prefixes, suffixes = 1 << (c * p), 1 << (c * (w - p))
    signature_bytes = (suffixes + 7) // 8
    return {"prefixes": prefixes, "suffixes": suffixes, "rows": prefixes * suffixes,
            "predicate_evaluations": 2 * prefixes * suffixes,
            "bytes_per_signature": signature_bytes,
            # Stored signatures, original/round-tripped decoder tables, buffers
            # and constant signatures. Text/hex and Python object overhead are separate.
            "signature_payload_upper_bound_bytes": 8 * prefixes * signature_bytes}


def signatures_for(case, budget):
    """Internal enumeration after checked_plan(); one call per input row."""
    np, ns, size = budget["prefixes"], budget["suffixes"], budget["bytes_per_signature"]
    signatures = []
    for prefix in range(np):
        signature = bytearray(size)
        for suffix in range(ns):
            if predicate(case, inputs(case, prefix, suffix)):
                signature[suffix // 8] |= 1 << (suffix % 8)
        signatures.append(bytes(signature))
    return signatures


def first_difference(left: bytes, right: bytes) -> int:
    """Least suffix ID at which two complete bit signatures differ."""
    for byte_index, (a, b) in enumerate(zip(left, right, strict=True)):
        different = a ^ b
        if different:
            return 8 * byte_index + (different & -different).bit_length() - 1
    raise ValueError("equal signatures have no distinguishing suffix")


def check_encoding(case, states: Sequence[int], state_bits: int, *,
                   conditional: bool = False, limits=None):
    """Exhaustively check a proposed fixed-width prefix-state encoder.

    The caller supplies one code per prefix. Conditional mode receives the
    exact canonical L,Q masks separately and ignores codes on resolved prefixes.
    All prefixes have the SAME full suffix domain; restricted valid domains
    require another problem formulation. A collision includes an arithmetic
    replay, not merely a disagreement between hash values.
    """
    if not isinstance(case, dict):
        raise ValueError("case must be a dictionary")
    # Own the validated semantic inputs before any predicate evaluation. The
    # integer/string fields are immutable; later caller mutations cannot relabel
    # an already-computed result or change the function during enumeration.
    case = dict(case)
    budget, limits = checked_plan(case, limits=limits)
    np, ns = budget["prefixes"], budget["suffixes"]
    if type(state_bits) is not int or not 0 <= state_bits <= 64:
        raise ValueError("candidate state width must be an integer in 0..64")
    if type(conditional) is not bool:
        raise ValueError("conditional must be a Boolean")
    if not isinstance(states, Sequence) or isinstance(states, (str, bytes, bytearray)) or len(states) != np:
        raise ValueError("candidate requires one integer code for every prefix")
    # Snapshot exactly the admitted prefix count. Indexed collection is bounded
    # even for a custom Sequence whose iterator would produce extra elements.
    states = tuple(states[index] for index in range(np))
    maximum = 1 << state_bits
    if any(type(code) is not int or not 0 <= code < maximum for code in states):
        raise ValueError("candidate code does not fit its declared state width")
    signatures = signatures_for(case, budget)
    size = budget["bytes_per_signature"]
    zero, one = bytes(size), ((1 << ns) - 1).to_bytes(size, "little")
    representatives = {}
    compared = active = 0
    result = {"case": dict(case), "preflight": budget, "limits": limits,
              "interface": "D(L,Q,E(prefix),suffix)" if conditional else "D(E(prefix),suffix)",
              "suffix_domain": "common full Cartesian domain",
              "candidate_state_bits": state_bits,
              "exact_optimum_claimed": False,
              "predicate_evaluations": budget["rows"]}
    for prefix, signature in enumerate(signatures):
        if conditional and signature in (zero, one):
            continue
        active += 1
        code = states[prefix]
        if code not in representatives:
            representatives[code] = prefix
            continue
        previous = representatives[code]
        compared += 1
        if signature == signatures[previous]:
            continue
        suffix = first_difference(signatures[previous], signature)
        left, right = inputs(case, previous, suffix), inputs(case, prefix, suffix)
        left_result, right_result = predicate(case, left), predicate(case, right)
        if left_result == right_result:
            raise AssertionError("collision witness failed arithmetic replay")
        result.update(status="ENCODING_COLLISION", exact_encoding=False,
                      predicate_evaluations=budget["rows"] + 2,
                      prefixes_visited=prefix + 1, active_prefixes_visited=active,
                      signature_comparisons=compared,
                      witness={"left_prefix": previous, "right_prefix": prefix,
                               "shared_state": code, "suffix": suffix,
                               "left_values": left, "right_values": right,
                               "left_result": bool(left_result), "right_result": bool(right_result),
                               "side_information": {"L": 0, "Q": 1} if conditional else None,
                               "arithmetic_replay_passed": True})
        return result
    result.update(status="EXACT_ENCODING", exact_encoding=True,
                  prefixes_visited=np, active_prefixes_visited=active,
                  signature_comparisons=compared, distinct_used_states=len(representatives),
                  witness=None)
    return result


def analyze(case, budget=None, *, limits=None):
    if not isinstance(case, dict):
        raise ValueError("case must be a dictionary")
    case = dict(case)
    # The old cached-plan argument is accepted, but never trusted as authority.
    # This owned case is used for computation and retained result provenance.
    budget, _ = checked_plan(case, budget, limits)
    np, ns, size = budget["prefixes"], budget["suffixes"], budget["bytes_per_signature"]
    signatures = signatures_for(case, budget)
    # Python hashes accelerate lookup, but bytes equality decides every class.
    # IDs are deterministic lexicographic ordering of the COMPLETE signatures.
    unique = sorted(set(signatures))
    ids = {signature: i for i, signature in enumerate(unique)}
    classes = [ids[signature] for signature in signatures]
    counts, representatives = [0] * len(unique), [None] * len(unique)
    zero, one = bytes(size), ((1 << ns) - 1).to_bytes(size, "little")
    unresolved = [i for i, signature in enumerate(unique) if signature not in (zero, one)]
    unresolved_ids = {full: i for i, full in enumerate(unresolved)}
    lower = [int(signature == one) for signature in signatures]
    care = [int(signature not in (zero, one)) for signature in signatures]
    pending = [unresolved_ids[cls] for cls, q in zip(classes, care) if q]
    for prefix, cls in enumerate(classes):
        counts[cls] += 1
        if representatives[cls] is None:
            representatives[cls] = prefix
    full_bits, conditional_bits = width_for(len(unique)), width_for(len(unresolved))
    full_encoder = pack(classes, full_bits)
    conditional_encoder = pack(pending, conditional_bits)
    lower_map, care_map = pack(lower, 1), pack(care, 1)
    full_decoder = b"".join(unique)
    conditional_decoder = b"".join(unique[i] for i in unresolved)
    representation = {
        "encoding": "LSB-first within bytes; channel0 is least-significant field; signature bit=suffix ID",
        "full": {"state_bits": full_bits, "encoder_hex": full_encoder.hex(),
                 "decoder_hex": full_decoder.hex()},
        "conditional": {"state_bits": conditional_bits, "lower_L_hex": lower_map.hex(),
                        "unresolved_Q_hex": care_map.hex(), "active_encoder_hex": conditional_encoder.hex(),
                        "decoder_hex": conditional_decoder.hex(), "class_ids_in_decoder_order": unresolved,
                        "active_prefix_count": len(pending),
                        "lookup": "Q=0 returns L; Q=1 uses rank1(Q,prefix)-1 as active encoder index"}}
    # Verify bytes round-tripped through the actual published JSON representation.
    full = representation["full"]
    conditional = representation["conditional"]
    ef, df = bytes.fromhex(full["encoder_hex"]), bytes.fromhex(full["decoder_hex"])
    ec, dc = bytes.fromhex(conditional["active_encoder_hex"]), bytes.fromhex(conditional["decoder_hex"])
    lm, qm = bytes.fromhex(conditional["lower_L_hex"]), bytes.fromhex(conditional["unresolved_Q_hex"])
    active, verified = 0, 0
    for prefix in range(np):
        cls = unpack(ef, prefix, full_bits)
        q, l = unpack(qm, prefix, 1), unpack(lm, prefix, 1)
        conditional_cls = unpack(ec, active, conditional_bits) if q else 0
        active += q
        for suffix in range(ns):
            expected = predicate(case, inputs(case, prefix, suffix))
            actual_full = (df[cls * size + suffix // 8] >> (suffix % 8)) & 1
            actual_conditional = ((dc[conditional_cls * size + suffix // 8] >> (suffix % 8)) & 1) if q else l
            if actual_full != expected or actual_conditional != expected:
                raise AssertionError((case["name"], prefix, suffix, "decoder mismatch"))
            verified += 1
    assert active == len(pending) and verified == budget["rows"]
    return {"case": case, "status": "EXACT_EXHAUSTIVE", "preflight": budget,
            "classes": [{"id": i, "representative_prefix": representatives[i],
                         "representative_prefix_fields": fields(representatives[i], case["prefix_bits"], case["channels"]),
                         "prefix_count": counts[i],
                         "constant": 0 if signature == zero else 1 if signature == one else None,
                         "signature_hex": signature.hex()} for i, signature in enumerate(unique)],
            "full_class_count": len(unique), "unresolved_class_count": len(unresolved),
            "minimum_full_fixed_width_state_bits": full_bits,
            "minimum_unresolved_fixed_width_state_bits": conditional_bits,
            "resolved_false_prefixes": sum(s == zero for s in signatures),
            "resolved_true_prefixes": sum(lower),
            "verified_rows_per_decoder": verified,
            "accounting": {"raw_prefix_bits": case["channels"] * case["prefix_bits"],
                           "full_encoder_bytes": len(full_encoder), "full_decoder_bytes": len(full_decoder),
                           "conditional_L_Q_bytes": len(lower_map) + len(care_map),
                           "conditional_active_encoder_bytes": len(conditional_encoder),
                           "conditional_decoder_bytes": len(conditional_decoder),
                           "conditional_rank_index_bytes": 0,
                           "scope": "Logical packed payloads only; no rank index (scan Q), Python objects, JSON/hex expansion, code or allocator/RSS claim."},
            "representation": representation}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("results/residual_quotient_v0_9/result.json"))
    parser.add_argument("--case", action="append", choices=[c["name"] for c in CASES])
    parser.add_argument("--max-rows", type=int, default=65536, help="maximum input rows per case")
    parser.add_argument("--max-evaluations", type=int, default=1_000_000, help="total generation plus verification calls")
    parser.add_argument("--max-signature-bytes", type=int, default=1_000_000, help="sum of conservative binary-signature payload bounds")
    args = parser.parse_args()
    selected = [c for c in CASES if args.case is None or c["name"] in args.case]
    plans = [plan(c) for c in selected]
    limits = {"max_rows_per_case": args.max_rows, "max_total_evaluations": args.max_evaluations,
              "max_signature_payload_bytes": args.max_signature_bytes}
    totals = {"evaluations": sum(p["predicate_evaluations"] for p in plans),
              "signature_payload_upper_bound_bytes": sum(p["signature_payload_upper_bound_bytes"] for p in plans)}
    errors = []
    if min(limits.values()) < 0:
        errors.append("budgets must be nonnegative")
    if any(p["rows"] > args.max_rows for p in plans):
        errors.append("row budget exceeded")
    if totals["evaluations"] > args.max_evaluations:
        errors.append("evaluation budget exceeded")
    if totals["signature_payload_upper_bound_bytes"] > args.max_signature_bytes:
        errors.append("signature payload budget exceeded")
    result = {"schema_version": 1, "production_runtime": False,
              "source_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "model": "exact deterministic decoder D(E(prefix),suffix); conditional mode also receives L and Q; no prefix rereads",
              "class_equality": "complete truth-vector bytes equality, never hash-digest equality",
              "canonical_ids": "lexicographic signature-byte order; smallest prefix ID represents each class",
              "limits": limits, "preflight_totals": totals,
              "primary_source": "https://www.cs.cmu.edu/~bryant/pubdir/ieeetc86.pdf"}
    if errors:
        result.update(status="BUDGET_REJECTED", exact_optimum_claimed=False, errors=errors, cases=[])
    else:
        result.update(status="PASS", exact_optimum_claimed="fixed-width interface-state bits only",
                      cases=[analyze(c, p, limits=limits) for c, p in zip(selected, plans)])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"status": result["status"], "cases": len(result["cases"]), "output": str(args.output), **totals}))
    return 2 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
