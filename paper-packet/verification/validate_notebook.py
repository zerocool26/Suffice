#!/usr/bin/env python3
"""Execute every notebook code cell in order, using only Python's standard library."""
from __future__ import annotations
import argparse
import contextlib
from datetime import datetime, timezone
import hashlib
import io
import json
from pathlib import Path
import sys
import time


def sha(data):
    return hashlib.sha256(data).hexdigest()


def sources(notebook):
    payload = [{"type":c["cell_type"],"source":c["source"]} for c in notebook["cells"]]
    return sha(json.dumps(payload,sort_keys=True,ensure_ascii=False).encode())


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--notebook",type=Path,required=True)
    parser.add_argument("--receipt",type=Path,required=True)
    parser.add_argument("--audit-report",type=Path,required=True)
    args=parser.parse_args()
    for path in (args.receipt,args.audit_report):
        if path.exists():raise ValueError("output exists: "+str(path))
    notebook=json.loads(args.notebook.read_text(encoding="utf-8"));before=sources(notebook)
    observed={"network_events":0,"external_process_events":0}
    def guard(event,arguments):
        if event.startswith("socket."):
            observed["network_events"]+=1;raise RuntimeError("network event during notebook validation")
        if event in {"subprocess.Popen","os.system","os.exec","os.posix_spawn","os.spawn","os.fork","os.forkpty","pty.spawn"}:
            observed["external_process_events"]+=1;raise RuntimeError("process launch during notebook validation")
    sys.addaudithook(guard)
    start=datetime.now(timezone.utc).isoformat();clock=time.perf_counter();cell_receipts=[];execution=0
    rich=[]
    def display(data,raw=False):
        if not raw or not isinstance(data,dict):raise ValueError("unexpected display protocol")
        if set(data)-{"text/markdown","image/svg+xml","text/plain"}:raise ValueError("unsupported display type")
        if sum(len(v) for v in data.values())>150000:raise ValueError("display output exceeds cap")
        rich.append({"output_type":"display_data","data":data,"metadata":{}})
    namespace={"__name__":"__suffice_notebook__","display":display}
    for index,cell in enumerate(notebook["cells"]):
        if cell["cell_type"]!="code":continue
        execution+=1;stdout=io.StringIO();stderr=io.StringIO();rich.clear();source="".join(cell["source"]);started=time.perf_counter()
        with contextlib.redirect_stdout(stdout),contextlib.redirect_stderr(stderr):
            exec(compile(source,f"Suffice_013_Verification.cell_{index}","exec"),namespace)
        if len(stdout.getvalue())+len(stderr.getvalue())>65536:raise ValueError("unexpected large cell output")
        cell["execution_count"]=execution;cell["outputs"]=[]
        for name,stream in (("stdout",stdout),("stderr",stderr)):
            if stream.getvalue():cell["outputs"].append({"output_type":"stream","name":name,"text":stream.getvalue().splitlines(keepends=True)})
        cell["outputs"].extend(rich)
        cell_receipts.append({"cell_index":index,"execution_count":execution,"status":"passed","source_sha256":sha(source.encode()),
                              "wall_ms":(time.perf_counter()-started)*1000,"stdout":stdout.getvalue(),"stderr":stderr.getvalue(),
                              "display_outputs":len(rich)})
    result=namespace["AUDIT_RESULT"];result["derived_table"]=namespace["RESULT_TABLE"]
    if result["status"]!="passed" or before!=sources(notebook):raise ValueError("audit/source identity mismatch")
    if observed!={"network_events":0,"external_process_events":0}:raise ValueError("forbidden external action attempted")
    notebook_bytes=(json.dumps(notebook,indent=1,ensure_ascii=False)+"\n").encode();args.notebook.write_bytes(notebook_bytes)
    audit_bytes=(json.dumps(result,indent=2,sort_keys=True)+"\n").encode()
    args.audit_report.parent.mkdir(parents=True,exist_ok=True)
    with args.audit_report.open("xb") as f:f.write(audit_bytes)
    receipt={"schema_version":1,"status":"passed","started_utc":start,"finished_utc":datetime.now(timezone.utc).isoformat(),
             "wall_ms":(time.perf_counter()-clock)*1000,"python":sys.version,"platform":sys.platform,
             "execution_method":"stdlib driver executed every code cell in order; captured stream and rich outputs",
             "jupyter_or_kaggle_ui_execution":False,"publicly_posted":False,
             "notebook":args.notebook.name,"notebook_bytes":len(notebook_bytes),"notebook_sha256":sha(notebook_bytes),
             "notebook_source_identity_before":before,"notebook_source_identity_after":sources(notebook),"code_cells_executed":execution,
             "external_action_audit":observed,"auditor_execution_guard":result["auditor_execution_guard"],
             "audit_report":args.audit_report.name,"audit_report_sha256":sha(audit_bytes),
             "embedded_archive_sha256":sha(namespace["ARCHIVE_BYTES"]),"included_auditor_sha256":result["audit_script_sha256"],
             "counts":result["counts"],"archive_files":result["archive_files"],"input_files":result["input_files"],
             "frozen_source_files":result["frozen_source_files"],"paired_blocks_per_scenario":10,
             "native_suffice_executables_invoked":False,"historical_native_outputs_reexecuted":False,
             "payload_frames_redecompressed":False,"model_calls_performed":False,"cells":cell_receipts}
    args.receipt.parent.mkdir(parents=True,exist_ok=True)
    with args.receipt.open("x",encoding="utf-8",newline="\n") as f:json.dump(receipt,f,indent=2,sort_keys=True);f.write("\n")
    svg=namespace["RESULT_SVG"]
    svg_path=args.receipt.parent/"Suffice_0_13_Paired_Observations.svg"
    with svg_path.open("x",encoding="utf-8",newline="\n") as f:f.write(svg+"\n")
    print(json.dumps({"status":"passed","code_cells":execution,"notebook_sha256":receipt["notebook_sha256"],
                      "counts":result["counts"],"external_action_audit":observed,"auditor_execution_guard":result["auditor_execution_guard"]}))


if __name__=="__main__":main()
