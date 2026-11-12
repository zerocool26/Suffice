#!/usr/bin/env python3
"""Real native bridge correctness checks; no model or performance experiment."""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import sys
import time
import traceback


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--packer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    runtime = args.runtime.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    sys.path.insert(0, str(runtime / "scripts"))
    from catalog_client import CatalogProcess
    import artifact_catalog
    from repository_read_bridge import RepositoryReadBridge

    class FaultClient(CatalogProcess):
        fault = None
        prefix_rows = 0
        prefix_body_bytes = 0

        def run(self, *values, **options):
            if self.fault == "prefix":
                def fail(row):
                    self.prefix_rows += 1
                    self.prefix_body_bytes += len(row["data"])
                    raise RuntimeError("injected failure after valid native row")
                options["on_row"] = fail
            result, rows = super().run(*values, **options)
            if self.fault == "identity":
                current = result["generation"]
                result = dict(result, generation=("0" if current[0] != "0" else "1") + current[1:])
            return result, rows

    originals = {
        "00_empty.txt": b"",
        "10_text.txt": b"first line\nsecond line\r\n",
        "20_binary.bin": b"\xff\x80" + bytes(range(256)),
        "unicode/\u96ea.txt": "naïve π 雪\n".encode("utf-8") + b"\x00",
    }
    changed = dict(originals)
    changed["10_text.txt"] = b"changed content\n"
    changed["21_renamed.bin"] = changed.pop("20_binary.bin")
    changed["new.txt"] = b"new file\n"
    tracked = [
        Path(__file__).resolve(), Path(__file__).with_name("repository_read_bridge.py").resolve(),
        args.binary.resolve(), args.packer.resolve(), runtime / "scripts/catalog_client.py",
        runtime / "scripts/artifact_catalog.py", runtime / "scripts/payload_store.py",
    ]
    before = {str(p): sha(p) for p in tracked}
    counts = {"native_processes": 0, "checks": 0, "complete_observations": 0,
              "verified_rows": 0, "verified_body_bytes": 0, "expected_rejections": 0,
              "injected_prefix_failures": 0, "injected_prefix_body_bytes": 0, "identity_failures": 0,
              "cross_transport_equalities": 0}
    report = {"schema_version": 1, "status": "running",
              "scope": "Real capture/native/client/read-bridge correctness; no Gemma inference or timing claim",
              "model_inference_executed": False, "native_windows_tested": os.name == "nt",
              "inputs_before": before, "cases": [], "arms": []}
    started = time.perf_counter()

    def check(label, condition):
        if not condition:
            raise AssertionError(label)
        counts["checks"] += 1
        report["cases"].append(label)

    def decode_rows(observation):
        values = []
        for row in observation["rows"]:
            content = row["content"]
            body = (content["text"].encode("utf-8") if content["encoding"] == "utf8"
                    else base64.b64decode(content["data"], validate=True))
            check("body_descriptor_matches", len(body) == row["bytes"]
                  and hashlib.sha256(body).hexdigest() == row["sha256"])
            values.append({"row": row["row"], "path": row["path"], "data": body})
        return values

    def expected(bodies):
        return [{"row": i, "path": name, "data": bodies[name]}
                for i, name in enumerate(sorted(bodies, key=lambda x: x.encode("utf-8")))]

    rendered = {}
    try:
        for name, bodies in (("initial", originals), ("changed", changed)):
            source = output / name / "source"
            for path, body in bodies.items():
                target = source / path
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(body)
            capture = artifact_catalog.capture(source, output / name / "store", args.packer.resolve())
            report[name + "_capture"] = capture
        old_rows, new_rows = expected(originals), expected(changed)
        for transport in ("jsonl", "binary"):
            arm = {"transport": transport, "successful_observation_sha256": {}}
            report["arms"].append(arm)
            client = FaultClient(args.binary, transport=transport)
            counts["native_processes"] += 1
            with client:
                opened = client.open("repo", output / "initial/store", expected_revision=0)
                bridge = RepositoryReadBridge(client, max_rows=8, max_bytes=4096)

                def bind(scope, alias, state=opened, historical=False):
                    bridge.bind(scope, "repo", alias, generation=state["generation"],
                                revision=state["revision"], rows=state["rows"],
                                allow_retired=historical)

                bind("primary", "approved")
                bind("history", "historypin", historical=True)
                schema = bridge.function_schema()
                check("schema_disallows_extra_arguments",
                      schema["function"]["parameters"]["additionalProperties"] is False
                      and "path" not in schema["function"]["parameters"]["properties"])

                def call(label, wanted, scope="primary", state=opened, first=0,
                         count=None, max_bytes=4096, retired=False):
                    text = bridge.invoke({"scope": scope, "generation": state["generation"],
                                          "first": first, "count": len(wanted) if count is None else count,
                                          "max_bytes": max_bytes})
                    result = json.loads(text)
                    check(label + "_complete", result["status"] == "ok"
                          and result["generation"] == state["generation"]
                          and result["revision"] == state["revision"]
                          and result["retired_generation"] is retired
                          and result["consistency"] == "pinned_generation"
                          and decode_rows(result) == wanted)
                    counts["complete_observations"] += 1
                    counts["verified_rows"] += len(wanted)
                    counts["verified_body_bytes"] += sum(len(r["data"]) for r in wanted)
                    arm["successful_observation_sha256"][label] = hashlib.sha256(text.encode()).hexdigest()
                    if transport == "jsonl":
                        rendered[label] = text
                    else:
                        check(label + "_transport_independent", text == rendered[label])
                        counts["cross_transport_equalities"] += 1
                    return result

                def reject(label, arguments, code, no_io=False):
                    sequence = client.sequence
                    result = json.loads(bridge.invoke(arguments))
                    check(label, result == {"schema": "suffice.repository_observation.v1",
                                           "status": "error", "error": {"code": code}})
                    if no_io:
                        check(label + "_no_native_request", client.sequence == sequence)
                    counts["expected_rejections"] += 1

                base = {"scope": "primary", "generation": opened["generation"],
                        "first": 0, "count": 4, "max_bytes": 4096}
                full = call("full_original", old_rows)
                check("utf8_and_base64_tags",
                      {r["content"]["encoding"] for r in full["rows"]} == {"utf8", "base64"})
                call("binary_row_range", old_rows[2:3], first=2)
                call("empty_body_zero_budget", old_rows[:1], max_bytes=0)
                call("zero_rows", [], first=len(old_rows), count=0, max_bytes=0)
                limited = RepositoryReadBridge(client, max_rows=8, max_bytes=4096,
                                               max_observation_bytes=1024)
                limited.bind("limited", "repo", "limitedpin", generation=opened["generation"],
                             revision=opened["revision"], rows=opened["rows"])
                limited_result = json.loads(limited.invoke(dict(base, scope="limited")))
                check("rendered_observation_budget_rejected", limited_result == {
                    "schema": "suffice.repository_observation.v1", "status": "error",
                    "error": {"code": "observation_budget_exceeded"}})
                counts["expected_rejections"] += 1
                for label, edits, code in (
                    ("wrong_scope", {"scope": "unapproved"}, "unknown_scope"),
                    ("wrong_generation", {"generation": "0" * 32}, "generation_mismatch"),
                    ("boolean_count", {"count": True}, "invalid_arguments"),
                    ("negative_first", {"first": -1}, "invalid_arguments"),
                    ("float_byte_budget", {"max_bytes": 2.0}, "invalid_arguments"),
                    ("oversized_row_budget", {"count": 9}, "invalid_arguments"),
                    ("oversized_byte_budget", {"max_bytes": 4097}, "invalid_arguments"),
                    ("out_of_range", {"first": 4, "count": 1}, "invalid_range"),
                    ("path_argument", {"path": "../private"}, "invalid_arguments"),
                ):
                    reject(label, dict(base, **edits), code, no_io=True)
                reject("missing_argument", {k: v for k, v in base.items() if k != "count"},
                       "invalid_arguments", no_io=True)
                reject("native_body_budget", dict(base, max_bytes=1), "native_rejected")
                check("native_budget_error_keeps_client_synchronized", client.stats()["status"] == "ok")
                client.unpin("approved")
                reject("unknown_native_pin", base, "native_rejected")
                client.pin("approved", "repo")  # Host explicitly restores its pin.
                current = client.open("repo", output / "changed/store", expected_revision=opened["revision"])
                reject("retired_pin_default_rejection", base, "retired_generation")
                call("explicit_historical_read", old_rows, scope="history", retired=True)
                bind("current", "currentpin", current)
                call("new_generation", new_rows, scope="current", state=current)
                for field, value in (("generation", "0" * 32), ("revision", current["revision"] + 1)):
                    approval = {"generation": current["generation"], "revision": current["revision"],
                                "rows": current["rows"]}
                    approval[field] = value
                    try:
                        bridge.bind("badapproval", "repo", "unusedpin", **approval)
                    except ValueError:
                        counts["expected_rejections"] += 1
                        check("host_" + field + "_approval_rejected", True)
                    else:
                        raise AssertionError("wrong host approval accepted")
                client.fault = "identity"
                reject("terminal_identity_mismatch",
                       dict(base, scope="current", generation=current["generation"], count=len(new_rows)),
                       "identity_mismatch")
                counts["identity_failures"] += 1
                check("identity_mismatch_closes_client", client.closed)

            # Inject an exception after receiving an actual native row. The
            # ordinary client closes on callback failure; no success/partial
            # observation is allowed to escape from the bridge.
            broken = FaultClient(args.binary, transport=transport)
            counts["native_processes"] += 1
            with broken:
                state = broken.open("repo", output / "initial/store", expected_revision=0)
                bridge = RepositoryReadBridge(broken, max_rows=8, max_bytes=4096)
                bridge.bind("primary", "repo", "faultpin", generation=state["generation"],
                            revision=state["revision"], rows=state["rows"])
                broken.fault = "prefix"
                failed = bridge.invoke({"scope": "primary", "generation": state["generation"],
                                        "first": 1, "count": 3, "max_bytes": 4096})
                check("native_prefix_not_published", json.loads(failed) == {
                    "schema": "suffice.repository_observation.v1", "status": "error",
                    "error": {"code": "delivery_failed"}})
                check("native_prefix_observed_then_child_closed",
                      broken.prefix_rows == 1 and broken.prefix_body_bytes > 0 and broken.closed)
                counts["injected_prefix_failures"] += 1
                counts["injected_prefix_body_bytes"] += broken.prefix_body_bytes
                counts["expected_rejections"] += 1
        report["status"] = "passed"
    except BaseException as error:
        report.update(status="failed", error=repr(error), traceback=traceback.format_exc())
        print(report["traceback"], file=sys.stderr)
    finally:
        after = {str(p): sha(p) for p in tracked}
        report.update(inputs_after=after, inputs_stable=before == after, counters=counts,
                      elapsed_ms=(time.perf_counter() - started) * 1000)
        if before != after:
            report.update(status="failed", identity_error="validation inputs changed")
        (output / "RESULT.json").write_text(json.dumps(report, indent=2, ensure_ascii=True) + "\n")
    print(json.dumps({"status": report["status"], "counters": counts}))
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
