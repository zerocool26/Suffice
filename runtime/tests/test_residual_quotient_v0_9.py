"""Finite interface-state proof obligations and preflight regression tests."""
from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import residual_quotient_lab as lab
import check_residual_encoding as checker


def case(name):
    return dict(next(c for c in lab.CASES if c["name"] == name))


class PreflightTests(unittest.TestCase):
    def setUp(self):
        self.case = case("signed_sum_two_p3")

    def assert_no_enumeration(self, function, error=ValueError):
        with patch.object(lab, "predicate", side_effect=AssertionError("predicate must not run")) as p:
            with self.assertRaises(error):
                function()
            p.assert_not_called()

    def test_default_imported_analysis_is_checked(self):
        result = lab.analyze(self.case)
        self.assertEqual((result["full_class_count"], result["unresolved_class_count"]), (3, 1))
        self.assertEqual(result["verified_rows_per_decoder"], 4096)

    def test_analysis_result_owns_case_provenance(self):
        c = {"name": "owned_case", "channels": 1, "width": 2, "prefix_bits": 1,
             "predicate": "sum", "threshold": 0}
        result = lab.analyze(c)
        self.assertIsNot(result["case"], c)
        c["threshold"] = 999
        self.assertEqual(result["case"]["threshold"], 0)
        self.assertEqual(result["full_class_count"], 2)
        result["case"]["threshold"] = -999
        self.assertEqual(c["threshold"], 999)

    def test_analysis_case_snapshot_precedes_enumeration(self):
        c = {"name": "owned_during_run", "channels": 1, "width": 2, "prefix_bits": 1,
             "predicate": "sum", "threshold": 0}
        reference = lab.predicate
        def mutate_caller(inner_case, values):
            c["threshold"] = 999
            return reference(inner_case, values)
        with patch.object(lab, "predicate", side_effect=mutate_caller):
            result = lab.analyze(c)
        self.assertEqual(result["case"]["threshold"], 0)
        self.assertEqual(result["full_class_count"], 2)
        self.assertEqual(c["threshold"], 999)

    def test_forged_cached_dimensions_rejected_before_work(self):
        for key in ("prefixes", "suffixes", "rows", "bytes_per_signature",
                    "predicate_evaluations", "signature_payload_upper_bound_bytes"):
            with self.subTest(key=key):
                forged = lab.plan(self.case)
                forged[key] += 1
                self.assert_no_enumeration(lambda: lab.analyze(self.case, forged))

    def test_direct_importer_cannot_skip_any_budget(self):
        for key in lab.DEFAULT_LIMITS:
            with self.subTest(key=key):
                self.assert_no_enumeration(
                    lambda: lab.analyze(self.case, limits={key: 0}), lab.BudgetRejected)

    def test_checker_cannot_skip_any_budget(self):
        for key in lab.DEFAULT_LIMITS:
            with self.subTest(key=key):
                self.assert_no_enumeration(
                    lambda: lab.check_encoding(self.case, [0] * 64, 0, limits={key: 0}),
                    lab.BudgetRejected)

    def test_malformed_case_dimensions_and_semantics_rejected(self):
        mutations = [("channels", True), ("channels", 5), ("width", 1),
                     ("prefix_bits", -1), ("prefix_bits", 7), ("width", 1000000000),
                     ("threshold", 0.5), ("predicate", "not_supported"), ("name", "")]
        for key, value in mutations:
            with self.subTest(key=key, value=value):
                altered = {**self.case, key: value}
                self.assert_no_enumeration(lambda: lab.analyze(altered))

    def test_invalid_limits_rejected(self):
        for limits in ({"max_rows_per_case": -1}, {"max_rows_per_case": True},
                       {"max_rows_per_case": float("inf")}, {"unlimited": 1}):
            self.assert_no_enumeration(lambda: lab.analyze(self.case, limits=limits))

    def test_raised_explicit_limits_are_honored(self):
        larger = {"name": "full_domain", "channels": 3, "width": 6,
                  "prefix_bits": 0, "predicate": "sum", "threshold": 0}
        self.assert_no_enumeration(lambda: lab.analyze(larger), lab.BudgetRejected)
        budget, _ = lab.checked_plan(larger, limits={"max_rows_per_case": 262144})
        self.assertEqual(budget["rows"], 262144)


class EncodingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.case = case("magnitude_four_p2")
        cls.analysis = lab.analyze(cls.case)
        cls.plan = lab.plan(cls.case)
        full = cls.analysis["representation"]["full"]
        encoded = bytes.fromhex(full["encoder_hex"])
        cls.states = [lab.unpack(encoded, i, full["state_bits"])
                      for i in range(cls.plan["prefixes"])]

    def test_checker_snapshots_case_and_state_codes_before_enumeration(self):
        c = {"name": "owned_encoder", "channels": 1, "width": 2, "prefix_bits": 1,
             "predicate": "sum", "threshold": 0}
        states = [1, 0]  # Different continuation rows for the two prefixes.
        reference = lab.predicate
        def mutate_caller(inner_case, values):
            c["threshold"] = 999
            states[:] = [1, 1]  # Invalid alias for the originally admitted function.
            return reference(inner_case, values)
        with patch.object(lab, "predicate", side_effect=mutate_caller):
            result = lab.check_encoding(c, states, 1)
        self.assertEqual(result["status"], "EXACT_ENCODING")
        self.assertEqual(result["case"]["threshold"], 0)
        self.assertEqual(result["distinct_used_states"], 2)
        self.assertEqual(c["threshold"], 999)
        self.assertEqual(states, [1, 1])

    def test_actual_eight_bit_encoding_is_sufficient(self):
        result = lab.check_encoding(self.case, self.states, 8)
        self.assertEqual(result["status"], "EXACT_ENCODING")
        self.assertEqual(result["prefixes_visited"], 256)
        self.assertEqual(result["distinct_used_states"], 195)
        self.assertFalse(result["exact_optimum_claimed"])

    def test_seven_bit_alias_has_concrete_arithmetic_counterexample(self):
        result = lab.check_encoding(self.case, [s & 127 for s in self.states], 7)
        self.assertEqual(result["status"], "ENCODING_COLLISION")
        w = result["witness"]
        self.assertNotEqual(w["left_prefix"], w["right_prefix"])
        self.assertEqual(self.states[w["left_prefix"]] & 127, self.states[w["right_prefix"]] & 127)
        self.assertNotEqual(sum(x*x for x in w["left_values"]) > 64,
                            sum(x*x for x in w["right_values"]) > 64)
        self.assertTrue(w["arithmetic_replay_passed"])
        self.assertLessEqual(result["predicate_evaluations"], self.plan["predicate_evaluations"])

    def test_conditional_seven_bit_alias_cannot_hide_behind_masks(self):
        result = lab.check_encoding(self.case, [s & 127 for s in self.states], 7, conditional=True)
        self.assertEqual(result["status"], "ENCODING_COLLISION")
        self.assertEqual(result["witness"]["side_information"], {"L": 0, "Q": 1})

    def test_zero_extra_bits_suffice_for_signed_sum_only_with_L_Q(self):
        c = case("signed_sum_two_p3")
        states = [0] * lab.plan(c)["prefixes"]
        self.assertEqual(lab.check_encoding(c, states, 0, conditional=True)["status"], "EXACT_ENCODING")
        self.assertEqual(lab.check_encoding(c, states, 0)["status"], "ENCODING_COLLISION")

    def test_resolved_conditional_states_are_irrelevant(self):
        c = case("constant_true_p2")
        states = [0] * lab.plan(c)["prefixes"]
        result = lab.check_encoding(c, states, 0, conditional=True)
        self.assertEqual(result["status"], "EXACT_ENCODING")
        self.assertEqual(result["active_prefixes_visited"], 0)

    def test_invalid_candidate_rejected_before_predicate_work(self):
        invalid = [([0] * 255, 8, False), ([256] * 256, 8, False),
                   ([True] * 256, 1, False), ([0] * 256, -1, False),
                   ([0] * 256, 65, False), ([0] * 256, 8, 1),
                   ((x for x in range(256)), 8, False)]
        for states, bits, conditional in invalid:
            with patch.object(lab, "predicate", side_effect=AssertionError("predicate must not run")) as p:
                with self.assertRaises(ValueError):
                    lab.check_encoding(self.case, states, bits, conditional=conditional)
                p.assert_not_called()

    def test_packed_document_matches_array_document(self):
        encoded = lab.pack(self.states, 8).hex()
        packed = checker.check_document({"case": self.case, "state_bits": 8, "encoder_hex": encoded})
        plain = checker.check_document({"case": self.case, "state_bits": 8, "states": self.states})
        self.assertEqual(packed, plain)

    def test_malformed_packed_document_rejected(self):
        docs = [{"case": self.case, "state_bits": 8, "encoder_hex": "00"},
                {"case": self.case, "state_bits": 8, "encoder_hex": "zz" * 256},
                {"case": self.case, "state_bits": 8, "encoder_hex": " " * 512},
                {"case": self.case, "state_bits": 8, "states": self.states, "encoder_hex": ""},
                {"case": self.case, "state_bits": 8, "states": self.states, "extra": True}]
        for doc in docs:
            with self.assertRaises(ValueError):
                checker.check_document(doc)

    def test_subbyte_packed_padding_rejected(self):
        c = {"name": "one_prefix", "channels": 1, "width": 2, "prefix_bits": 0,
             "predicate": "sum", "threshold": 0}
        self.assertEqual(checker.check_document({"case": c, "state_bits": 1,
                                                "encoder_hex": "00"})["status"], "EXACT_ENCODING")
        with self.assertRaises(ValueError):
            checker.check_document({"case": c, "state_bits": 1, "encoder_hex": "80"})
        with self.assertRaises(ValueError):
            checker.check_document({"case": c, "state_bits": 1, "encoder_hex": "  "})

    def test_prefix_and_suffix_endpoint_shapes(self):
        for p in (0, 4):
            c = {"name": f"end_{p}", "channels": 1, "width": 4, "prefix_bits": p,
                 "predicate": "sum", "threshold": 0}
            analysis = lab.analyze(c)
            full = analysis["representation"]["full"]
            states = [lab.unpack(bytes.fromhex(full["encoder_hex"]), i, full["state_bits"])
                      for i in range(analysis["preflight"]["prefixes"])]
            self.assertEqual(lab.check_encoding(c, states, full["state_bits"])["status"], "EXACT_ENCODING")


class MathematicalBoundaryTests(unittest.TestCase):
    def test_restricted_domain_compatibility_is_not_an_equivalence(self):
        # Every pair has a nonempty overlap, but agreement on overlaps is not transitive.
        rows = {"A": {0: 0, 1: 0}, "B": {1: 0, 2: 0}, "C": {0: 1, 2: 0}}
        def compatible(a, b):
            return all(rows[a][s] == rows[b][s] for s in rows[a].keys() & rows[b].keys())
        self.assertTrue(compatible("A", "B"))
        self.assertTrue(compatible("B", "C"))
        self.assertFalse(compatible("A", "C"))
        # Two codes suffice: B can share either A's or C's code; one code cannot.
        codes = {"A": 0, "B": 0, "C": 1}
        for a in rows:
            for b in rows:
                self.assertTrue(codes[a] != codes[b] or compatible(a, b))


class CliTests(unittest.TestCase):
    def test_cli_distinguishes_sufficient_collision_and_unknown(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            candidate, output = tmp / "candidate.json", tmp / "result.json"
            c = case("signed_sum_two_p3")
            for conditional, expected_exit, expected_status in ((True, 0, "EXACT_ENCODING"),
                                                                (False, 1, "ENCODING_COLLISION")):
                candidate.write_text(json.dumps({"case": c["name"], "conditional": conditional,
                                                 "state_bits": 0, "encoder_hex": ""}))
                proc = subprocess.run([sys.executable, str(ROOT / "scripts/check_residual_encoding.py"),
                                       "--input", str(candidate), "--output", str(output)],
                                      text=True, capture_output=True)
                self.assertEqual(proc.returncode, expected_exit, proc.stderr)
                self.assertEqual(json.loads(output.read_text())["status"], expected_status)
            proc = subprocess.run([sys.executable, str(ROOT / "scripts/check_residual_encoding.py"),
                                   "--input", str(candidate), "--output", str(output), "--max-rows", "0"],
                                  text=True, capture_output=True)
            result = json.loads(output.read_text())
            self.assertEqual(proc.returncode, 2, proc.stderr)
            self.assertEqual(result["status"], "BUDGET_REJECTED")
            self.assertEqual(result["proof_status"], "UNKNOWN")
            self.assertIsNone(result["exact_encoding"])
            self.assertFalse(result["exact_optimum_claimed"])


if __name__ == "__main__":
    unittest.main()
