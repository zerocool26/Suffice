"""Build and measure the complete Suffice 0.10 payload application.

Nine native candidates share the identical snapshot, codec and materializing
output sink. Each timed output is checked in full against original plaintext
after the timer. Results describe one host and synthetic/corpus workloads.
"""
from pathlib import Path
import argparse, collections, datetime, hashlib, json, math, os, platform, random
import statistics, struct, subprocess, sys, time
import numpy as np
from payload_store import write_store
from payload_oracle import inspect_case
ROOT=Path(__file__).resolve().parent.parent
NAMES=["eager","fused","fused-zoned","dense","wordcode","zoned","sqlite","cached","wordcode-duplicate"]
def now():return datetime.datetime.now(datetime.timezone.utc).isoformat()
def sha(p):
    h=hashlib.sha256()
    with Path(p).open("rb") as f:
        for b in iter(lambda:f.read(1<<20),b""):h.update(b)
    return h.hexdigest()
def write(path,value):Path(path).write_text(json.dumps(value,indent=2)+"\n")
def sources():
    paths=[]
    for directory,pattern in [("src","*.cpp"),("include","*.hpp"),("apps","*.cpp"),("apps","*.hpp"),
                               ("tests","*.cpp"),("scripts","*.py")]:
        paths.extend((ROOT/directory).rglob(pattern))
    paths.append(ROOT/"CMakeLists.txt")
    return {str(p.relative_to(ROOT)):sha(p) for p in sorted(set(paths))}
def environment():
    cpu="unknown"
    if Path("/proc/cpuinfo").exists():
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):cpu=line.split(":",1)[1].strip();break
    return {"cpu":cpu,"platform":platform.platform(),"machine":platform.machine(),
            "python":platform.python_version(),"affinity":sorted(os.sched_getaffinity(0)) if hasattr(os,"sched_getaffinity") else None}
def runtime_dependencies(benchmark):
    paths=set()
    if sys.platform.startswith("linux"):
        import ctypes
        import re
        output=subprocess.check_output(["ldd",str(benchmark)],text=True)
        for line in output.splitlines():
            match=re.search(r"(?:=>\s*)?(/[^\s]+)",line)
            if match and Path(match.group(1)).is_file():paths.add(Path(match.group(1)).resolve())
        library=ctypes.CDLL("libsqlite3.so.0")
        address=ctypes.cast(library.sqlite3_libversion,ctypes.c_void_p).value
        for line in Path("/proc/self/maps").read_text().splitlines():
            fields=line.split(maxsplit=5)
            low,high=(int(x,16) for x in fields[0].split("-"))
            if low<=address<high and len(fields)==6 and Path(fields[5]).is_file():
                paths.add(Path(fields[5]).resolve());break
        else:raise ValueError("cannot identify loaded native SQLite module")
    return {str(p):sha(p) for p in sorted(paths)}
def freeze_gate(suite,binary,benchmark):
    if suite["environment"]!=environment() or suite["sources"]!=sources():raise ValueError("measurement environment/source changed")
    if suite["binary_sha256"]!=sha(binary) or suite["benchmark_sha256"]!=sha(benchmark):raise ValueError("measurement executable changed")
    for path,digest in suite["runtime_dependencies"].items():
        if sha(path)!=digest:raise ValueError("runtime dependency changed")

def execute(argv,log):
    begin=time.perf_counter_ns()
    with Path(log).open("w") as stream:
        p=subprocess.run(list(map(str,argv)),stdout=stream,stderr=subprocess.STDOUT)
    if p.returncode:raise RuntimeError(f"command failed ({p.returncode}): {log}")
    return time.perf_counter_ns()-begin
def truth(features,query,threshold):
    if query=="and2" or query=="and8":return (features>threshold).all(axis=1)
    return ((features[:,0]>threshold)|(features[:,1]>threshold)) & ((features[:,2]>threshold)|(features[:,3]>threshold))
def definitions(rows):
    return [
      dict(name="and2_scattered_1pct_empty",query="and2",channels=2,fraction=.01,layout="scattered",payload_bytes=0),
      dict(name="and2_scattered_1pct_1k",query="and2",channels=2,fraction=.01,layout="scattered",payload_bytes=1024),
      dict(name="and2_clustered_1pct_1k",query="and2",channels=2,fraction=.01,layout="clustered",payload_bytes=1024),
      dict(name="and2_scattered_25pct_1k",query="and2",channels=2,fraction=.25,layout="scattered",payload_bytes=1024),
      dict(name="and2_clustered_25pct_1k",query="and2",channels=2,fraction=.25,layout="clustered",payload_bytes=1024),
      dict(name="and2_scattered_25pct_correlated_1k",query="and2",channels=2,fraction=.25,layout="scattered",payload_bytes=1024,rejection="mixed_channels"),
      dict(name="and2_clustered_25pct_correlated_1k",query="and2",channels=2,fraction=.25,layout="clustered",payload_bytes=1024,rejection="mixed_channels"),
      dict(name="and2_none_1k",query="and2",channels=2,fraction=0.,layout="scattered",payload_bytes=1024),
      dict(name="and2_all_1k",query="and2",channels=2,fraction=1.,layout="scattered",payload_bytes=1024),
      dict(name="and8_scattered_1pct_1k",query="and8",channels=8,fraction=.01,layout="scattered",payload_bytes=1024),
      dict(name="overlap4_scattered_25pct_1k",query="overlap4",channels=4,fraction=.25,layout="scattered",payload_bytes=1024),
      dict(name="and2_scattered_1pct_4k",query="and2",channels=2,fraction=.01,layout="scattered",payload_bytes=4096),
      dict(name="and2_scattered_25pct_4k",query="and2",channels=2,fraction=.25,layout="scattered",payload_bytes=4096),
      dict(name="source_artifacts",query="and2",channels=2,layout="corpus",payload_bytes=None),
    ]
def make_case(case,folder,rows,binary):
    folder.mkdir()
    threshold=1000
    begin=time.perf_counter_ns()
    if case["layout"]=="corpus":
        candidates=sorted(p for directory in ("src","include","examples") for p in (ROOT/directory).rglob("*")
                          if p.is_file() and p.suffix in (".cpp",".hpp",".suf") and p.stat().st_size<=1<<20)
        bodies=[p.read_bytes() for p in candidates]
        features=np.array([[len(b),b.count(b"\n")*64] for b in bodies],dtype="<i4")
        threshold=4096;identifiers=np.arange(len(bodies),dtype="<u8")
        records=[{"row":i,"path":str(p.relative_to(ROOT)),"sha256":hashlib.sha256(b).hexdigest(),
                  "bytes":len(b),"newlines":b.count(b"\n")} for i,(p,b) in enumerate(zip(candidates,bodies))]
        write(folder/"corpus.json",{"features":["byte_count","64 * newline_count"],"records":records})
        payloads=iter(bodies)
    else:
        # Same canonical records for matching query/fraction/size, independent
        # of physical layout; reordering preserves metadata+payload association.
        rng=np.random.default_rng(2026111200+case["channels"]*100+int(case["fraction"]*100))
        features=rng.integers(0,2001,size=(rows,case["channels"]),dtype=np.int32)
        accepted=np.arange(rows)<round(rows*case["fraction"])
        if case["query"] in ("and2","and8"):
            features[accepted]=rng.integers(1001,2001,size=(int(accepted.sum()),case["channels"]),dtype=np.int32)
            rejected=np.flatnonzero(~accepted)
            columns=np.zeros(len(rejected),dtype=np.int64) if case["query"]=="and2" and case.get("rejection")!="mixed_channels" else rng.integers(0,case["channels"],size=len(rejected))
            features[rejected,columns]=rng.integers(0,1001,size=len(rejected))
        else:
            yes=np.flatnonzero(accepted);no=np.flatnonzero(~accepted)
            features[yes,0]=rng.integers(1001,2001,size=len(yes))
            features[yes,2]=rng.integers(1001,2001,size=len(yes))
            features[no,0:2]=rng.integers(0,1001,size=(len(no),2))
        assert np.array_equal(truth(features,case["query"],threshold),accepted)
        identifiers=np.arange(rows,dtype="<u8")
        if case["layout"]=="scattered":
            order=np.random.default_rng(77331).permutation(rows)
            features=features[order];identifiers=identifiers[order]
        features=features.astype("<i4")
        size=case["payload_bytes"]
        def payloads_generator():
            for identifier in identifiers:
                # Deterministic diagnostic-text payload, distinct original event
                # identity retained across scattered/clustered reordering.
                line=(f"build event {int(identifier):08d}: compile source; exact diagnostic artifact; "
                      f"phase={int(identifier)%17}; dependency={int(identifier)%101}; status=complete\n").encode()
                yield (line*((size+len(line)-1)//len(line)))[:size]
        payloads=payloads_generator()
    raw=folder/"features.i32";features.tofile(raw);identifiers.tofile(folder/"event_ids.u64")
    stats=write_store(folder/"payload.spp",raw,case["channels"],payloads,folder/"plaintext.ref")
    construction_ns=time.perf_counter_ns()-begin
    pack_ns=execute([binary,"pack","--input",raw,"--output",folder/"features.sfp","--channels",case["channels"],"--block-rows","4096"],folder/"pack.log")
    selected=truth(features,case["query"],threshold)
    case.update(rows=len(features),threshold=threshold,first=0,count=len(features),
                selected_rows=int(selected.sum()),construction_ns=construction_ns,packing_ns=pack_ns,
                store=stats,inputs={p.name:sha(p) for p in folder.iterdir() if p.is_file()},runs=[])
    case["oracle"]=inspect_case(folder,case)
    if case["selected_rows"]!=case["oracle"]["selected_rows"]:raise ValueError("independent selection differs")
    return case
def orders_python(count,rounds,seed):
    mask=(1<<64)-1
    def random64():
        nonlocal seed
        seed=(seed+0x9e3779b97f4a7c15)&mask
        value=seed;value=((value^(value>>30))*0xbf58476d1ce4e5b9)&mask
        value=((value^(value>>27))*0x94d049bb133111eb)&mask
        return value^(value>>31)
    def shuffle(items):
        for left in range(len(items),1,-1):
            index=random64()%left;items[left-1],items[index]=items[index],items[left-1]
    base=[0 if c==0 else (c+1)//2 if c%2 else count-c//2 for c in range(count)]
    cycle=count if count%2==0 else count*2
    result=[]
    while len(result)<rounds:
        labels=list(range(count));row_order=list(range(cycle));shuffle(labels);shuffle(row_order)
        for row in row_order:
            if len(result)==rounds:break
            result.append([labels[(base[count-1-c if row>=count else c]+row%count)%count] for c in range(count)])
    return result
def validate(record,case,seed,rounds):
    if record["status"]!="PASS" or record["schema_version"]!=10:raise ValueError("status/schema")
    for key in ("rows","channels","query","threshold","first","count","selected_rows"):
        if record[key]!=case[key]:raise ValueError("case identity "+key)
    oracle=case["oracle"]
    for key in ("selected_rows","output_bytes","output_fnv64","zone_blocks","constant_decision"):
        if record[key]!=oracle[key]:raise ValueError("independent oracle differs: "+key)
    if record["seed"]!=seed or record["rounds"]!=rounds:raise ValueError("timing identity")
    if record["complete_output_checks"]!=9*(rounds+1):raise ValueError("output validation count")
    if not record["full_payload_validation"]:raise ValueError("payload validation policy")
    if [x["name"] for x in record["candidates"]]!=NAMES:raise ValueError("candidate inventory")
    if record["timing_order"]!=orders_python(9,rounds,seed):raise ValueError("schedule identity")
    positions=collections.Counter();predecessors=collections.Counter()
    for order in record["timing_order"]:
        if sorted(order)!=list(range(9)):raise ValueError("schedule permutation")
        for pos,c in enumerate(order):positions[pos,c]+=1
        predecessors.update(zip(order,order[1:]))
    if set(positions.values())!={rounds//9} or set(predecessors.values())!={rounds//9}:raise ValueError("schedule balance")
    for candidate in record["candidates"]:
        samples=candidate["samples_ns"]
        if len(samples)!=rounds or not all(type(s) is int and s>0 for s in samples):raise ValueError("timing samples")
        if statistics.median(samples)!=candidate["median_ns"]:raise ValueError("stored median")
        if candidate["selected_rows"]!=case["selected_rows"] or candidate["output_bytes"]!=record["output_bytes"]:raise ValueError("output accounting")
        expected_decodes=0 if candidate["name"]=="cached" else case["count"] if candidate["name"]=="eager" else case["selected_rows"]
        if candidate["decoded_rows"]!=expected_decodes:raise ValueError("decode row accounting")
    by_name={c["name"]:c for c in record["candidates"]}
    for key in ("compressed_bytes","decoded_bytes","decoded_rows","selected_rows"):
        values={by_name[n][key] for n in NAMES if n not in ("eager","cached")}
        if len(values)!=1:raise ValueError("selective payload accounting differs")
    if by_name["wordcode"]["decoded_bytes"]!=record["output_bytes"]:raise ValueError("decode/output byte accounting")
    if by_name["cached"]["decoded_bytes"] or by_name["cached"]["compressed_bytes"]:raise ValueError("cache decode claim")
    for name,candidate in by_name.items():
        expected_compressed=0 if name=="cached" else oracle["range_compressed_bytes"] if name=="eager" else oracle["selected_compressed_bytes"]
        expected_decoded=0 if name=="cached" else oracle["range_decoded_bytes"] if name=="eager" else oracle["selected_decoded_bytes"]
        if candidate["compressed_bytes"]!=expected_compressed or candidate["decoded_bytes"]!=expected_decoded:raise ValueError("independent payload accounting differs")
    for n in ("zoned","fused-zoned"):
        for key in ("zone_true_rows","zone_false_rows","zone_refined_rows"):
            if by_name[n][key]!=oracle[key]:raise ValueError("independent zone accounting differs")
        if sum(by_name[n][k] for k in ("zone_true_rows","zone_false_rows","zone_refined_rows"))!=case["count"]:raise ValueError("zone coverage")
    if [by_name["zoned"][k] for k in ("zone_true_rows","zone_false_rows","zone_refined_rows")]!=[by_name["fused-zoned"][k] for k in ("zone_true_rows","zone_false_rows","zone_refined_rows")]:raise ValueError("zone facts differ")
def validate_structure(suite,out):
    if suite.get("schema_version")!=10 or suite.get("status") not in ("prepared","complete"):
        raise ValueError("incomplete preparation or invalid status")
    config=suite["configuration"]
    if set(config)!={"rows","processes","rounds","cases"}:raise ValueError("configuration fields")
    if type(config["rows"]) is not int or not 1<=config["rows"]<=1<<20:raise ValueError("configured rows")
    if type(config["processes"]) is not int or not 1<=config["processes"]<=64:raise ValueError("configured processes")
    if type(config["rounds"]) is not int or not 0<config["rounds"]<=10000 or config["rounds"]%18:raise ValueError("configured rounds")
    if suite["processes"]!=config["processes"] or suite["rounds"]!=config["rounds"]:raise ValueError("configuration mismatch")
    expected=definitions(config["rows"])
    if config["cases"] is not None:
        names=config["cases"]
        if not isinstance(names,list) or len(names)!=len(set(names)):raise ValueError("configured case names")
        expected=[c for c in expected if c["name"] in names]
        if len(expected)!=len(names):raise ValueError("unknown configured case")
    if [c["name"] for c in suite["cases"]]!=[c["name"] for c in expected]:raise ValueError("case inventory differs")
    for case,definition in zip(suite["cases"],expected):
        if any(case[k]!=v for k,v in definition.items()):raise ValueError("case definition differs")
        if case.get("rejection","fixed_channel")!=definition.get("rejection","fixed_channel"):
            raise ValueError("case rejection policy differs")
        if case["first"]!=0 or case["count"]!=case["rows"]:raise ValueError("case extent")
        if case["layout"]!="corpus" and case["rows"]!=config["rows"]:raise ValueError("case row count")
        if case["threshold"]!=(4096 if case["layout"]=="corpus" else 1000):raise ValueError("case threshold")
        expected_inputs={"features.i32","features.sfp","event_ids.u64","payload.spp","plaintext.ref","pack.log"}
        if case["layout"]=="corpus":expected_inputs.add("corpus.json")
        if set(case["inputs"])!=expected_inputs:raise ValueError("input inventory differs")
        folder=out/"cases"/case["name"]
        if (folder/"features.i32").stat().st_size!=case["rows"]*case["channels"]*4:raise ValueError("raw extent differs")
        if (folder/"event_ids.u64").stat().st_size!=case["rows"]*8:raise ValueError("event identity extent")
        if not 0<=case["selected_rows"]<=case["rows"]:raise ValueError("selected extent")
def validate_retained(suite,out,complete):
    for index,case in enumerate(suite["cases"]):
        seen=set();identity=None
        for run in case["runs"]:
            process=run["process"]
            if type(process) is not int or process in seen or not 0<=process<suite["processes"]:
                raise ValueError("retained process inventory")
            seen.add(process)
            if run["path"]!=f"cases/{case['name']}/process{process}.json":raise ValueError("retained process path")
            path=out/run["path"]
            if sha(path)!=run["sha256"]:raise ValueError("retained measurement changed")
            record=json.loads(path.read_text())
            validate(record,case,0x5355464610000000+index*100+process,suite["rounds"])
            current=(record["output_fnv64"],record["output_bytes"],record["zone_blocks"],record["wordcode_nodes"],record["wordcode_planes"],
                     record["codec_version"],record["sqlite_version"])
            if identity is None:identity=current
            elif identity!=current:raise ValueError("retained output/plan identity differs")
        if complete and seen!=set(range(suite["processes"])):raise ValueError("retained complete process coverage")

def record_pair_checks(cases):
    groups=collections.defaultdict(dict)
    for case in cases:
        if case["layout"] not in ("scattered","clustered"):continue
        key=(case["query"],case["channels"],case["fraction"],case["payload_bytes"],case.get("rejection","fixed_channel"))
        group=groups[key]
        if case["layout"] in group:raise ValueError("duplicate paired layout")
        group[case["layout"]]=case
    checks=[]
    for group in groups.values():
        if set(group)!={"scattered","clustered"}:continue
        left,right=group["scattered"],group["clustered"]
        digest=left["oracle"]["canonical_record_sha256"]
        if digest!=right["oracle"]["canonical_record_sha256"]:raise ValueError("scattered/clustered complete-record multiset differs")
        checks.append({"left":left["name"],"right":right["name"],"canonical_record_sha256":digest})
    return checks
def verify_inputs(suite,out):
    for case in suite["cases"]:
        folder=out/"cases"/case["name"]
        for name,h in case["inputs"].items():
            if sha(folder/name)!=h:raise ValueError("input changed")
        if inspect_case(folder,case)!=case["oracle"]:raise ValueError("saved oracle differs from retained input")
    if record_pair_checks(suite["cases"])!=suite["record_pair_checks"]:raise ValueError("record pair inventory differs")
def summarize(suite,out):
    validate_structure(suite,out)
    if suite["status"]!="complete":raise ValueError("summary requires complete measurement")
    validate_retained(suite,out,complete=True)
    verify_inputs(suite,out)
    rows=[]
    for index,case in enumerate(suite["cases"]):
        if len(case["runs"])!=suite["processes"]:raise ValueError("incomplete processes")
        seen=set();observations=[]
        for run in case["runs"]:
            process=run["process"]
            if process in seen or not 0<=process<suite["processes"]:raise ValueError("process inventory")
            seen.add(process)
            expected_path=f"cases/{case['name']}/process{process}.json"
            if run["path"]!=expected_path:raise ValueError("measurement path")
            file=out/run["path"]
            if sha(file)!=run["sha256"]:raise ValueError("measurement changed")
            record=json.loads(file.read_text())
            seed=0x5355464610000000+index*100+process
            validate(record,case,seed,suite["rounds"])
            med={c["name"]:statistics.median(c["samples_ns"]) for c in record["candidates"]}
            ratios={"fused/wordcode":med["fused"]/med["wordcode"],"fused-zoned/zoned":med["fused-zoned"]/med["zoned"],
                    "fused/zoned":med["fused"]/med["zoned"],"eager/fused":med["eager"]/med["fused"],
                    "sqlite/zoned":med["sqlite"]/med["zoned"],"fused/cached":med["fused"]/med["cached"],
                    "wordcode/duplicate":med["wordcode"]/med["wordcode-duplicate"]}
            observations.append({"process":process,"medians_ns":med,"ratios":ratios})
        aggregate={}
        for key in observations[0]["ratios"]:
            values=[p["ratios"][key] for p in observations]
            aggregate[key]={"geomean":math.exp(sum(math.log(v) for v in values)/len(values)),
                            "min":min(values),"max":max(values),"each_process":values}
        first=json.loads((out/case["runs"][0]["path"]).read_text())
        rows.append({"case":case["name"],"rows":case["rows"],"selected_rows":case["selected_rows"],
                     "output_bytes":first["output_bytes"],"aggregates":aggregate,"observations":observations,
                     "median_of_process_medians_ns":{n:statistics.median(o["medians_ns"][n] for o in observations) for n in NAMES},
                     "work":{c["name"]:{k:v for k,v in c.items() if k not in ("name","samples_ns","median_ns")} for c in first["candidates"]},
                     "wordcode_nodes":first["wordcode_nodes"],"wordcode_planes":first["wordcode_planes"]})
    result={"status":"PASS","schema_version":10,"case_count":len(rows),"process_count":sum(len(c["runs"]) for c in suite["cases"]),
            "complete_output_checks":sum(9*(suite["rounds"]+1)*len(c["runs"]) for c in suite["cases"]),
            "scope":"Warm complete-output materialization, one host, six fresh processes by default. All payload candidates share codec/output semantics. Descriptive process ranges, not confidence intervals.",
            "cases":rows}
    write(out/"SUMMARY.json",result)
    lines=["# Suffice 0.10 complete payload results","",
      "Each ratio is the geometric mean of same-process control median / treatment median. Above 1 favors the denominator. Parentheses are observed process min/max, not confidence intervals.","",
      "| Case | Selected | Fused / Wordcode | Fused+zones / Zoned | Fused / Zoned | Eager / Fused | Wordcode / duplicate |",
      "|---|---:|---:|---:|---:|---:|---:|"]
    for row in rows:
        columns=[row["case"],str(row["selected_rows"])]
        for key in ("fused/wordcode","fused-zoned/zoned","fused/zoned","eager/fused","wordcode/duplicate"):
            v=row["aggregates"][key];columns.append(f'{v["geomean"]:.4f}× ({v["min"]:.3f}–{v["max"]:.3f})')
        lines.append("| "+" | ".join(columns)+" |")
    (out/"TABLES.md").write_text("\n".join(lines)+"\n")
    return result
def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--binary",type=Path,required=True)
    ap.add_argument("--benchmark",type=Path,required=True)
    ap.add_argument("--output",type=Path,required=True)
    ap.add_argument("--rows",type=int,default=32785)
    ap.add_argument("--processes",type=int,default=6)
    ap.add_argument("--rounds",type=int,default=18)
    ap.add_argument("--cases",nargs="+")
    ap.add_argument("--prepare-only",action="store_true")
    ap.add_argument("--resume",action="store_true")
    ap.add_argument("--summary-only",action="store_true")
    args=ap.parse_args()
    if not 1<=args.rows<=1<<20 or not 1<=args.processes<=64 or not 0<args.rounds<=10000 or args.rounds%18:raise ValueError("invalid experiment dimensions")
    if hasattr(os,"sched_getaffinity"):os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
    out=args.output.resolve();binary=args.binary.resolve();benchmark=args.benchmark.resolve()
    if args.summary_only:
        result=summarize(json.loads((out/"suite.json").read_text()),out)
        print(json.dumps({"status":result["status"],"cases":result["case_count"],"processes":result["process_count"]}));return
    configuration={"rows":args.rows,"processes":args.processes,"rounds":args.rounds,"cases":args.cases}
    if args.resume:
        suite=json.loads((out/"suite.json").read_text())
        validate_structure(suite,out)
        validate_retained(suite,out,complete=suite["status"]=="complete")
        if suite["configuration"]!=configuration:raise ValueError("resume configuration differs")
        if suite["sources"]!=sources():raise ValueError("source changed")
        if suite["environment"]!=environment():raise ValueError("environment changed")
        if suite["binary_sha256"]!=sha(binary) or suite["benchmark_sha256"]!=sha(benchmark):raise ValueError("binary changed")
        verify_inputs(suite,out)
        freeze_gate(suite,binary,benchmark)
        if suite["status"]=="complete":
            summarize(suite,out)
            print("Already complete; retained measurements verified");return
    else:
        out.mkdir(parents=True,exist_ok=False);(out/"cases").mkdir()
        cases=definitions(args.rows)
        if args.cases:
            cases=[c for c in cases if c["name"] in set(args.cases)]
            if len(cases)!=len(args.cases):raise ValueError("case selection differs")
        suite={"schema_version":10,"created_utc":now(),"status":"preparing","configuration":configuration,
               "processes":args.processes,"rounds":args.rounds,"sources":sources(),"environment":environment(),
               "binary_sha256":sha(binary),"benchmark_sha256":sha(benchmark),"runtime_dependencies":runtime_dependencies(benchmark),"cases":[]}
        write(out/"suite.json",suite)
        for case in cases:
            suite["cases"].append(make_case(case,out/"cases"/case["name"],args.rows,binary))
            write(out/"suite.json",suite);print("Prepared "+case["name"],flush=True)
        suite["record_pair_checks"]=record_pair_checks(suite["cases"])
        if suite["sources"]!=sources():raise ValueError("source changed during preparation")
        suite["status"]="prepared";suite["prepared_utc"]=now();write(out/"suite.json",suite)
    if args.prepare_only:return
    jobs=[(i,p) for i in range(len(suite["cases"])) for p in range(args.processes)]
    random.Random(2026111210).shuffle(jobs)
    suite["measurement_started_utc"]=suite.get("measurement_started_utc",now());write(out/"suite.json",suite)
    for index,process in jobs:
        case=suite["cases"][index]
        if any(r["process"]==process for r in case["runs"]):continue
        freeze_gate(suite,binary,benchmark)
        folder=out/"cases"/case["name"];path=folder/f"process{process}.json";log=folder/f"process{process}.stderr.log"
        seed=0x5355464610000000+index*100+process
        argv=[benchmark,folder/"features.i32",folder/"features.sfp",folder/"payload.spp",folder/"plaintext.ref",
              case["channels"],case["query"],case["threshold"],case["first"],case["count"],args.rounds,seed]
        began=now();timer=time.perf_counter_ns()
        with path.open("x") as stdout,log.open("x") as stderr:
            run=subprocess.run(list(map(str,argv)),stdout=stdout,stderr=stderr)
        wall=time.perf_counter_ns()-timer
        if run.returncode:raise RuntimeError(f"native process failed: {path}, {log}")
        freeze_gate(suite,binary,benchmark)
        record=json.loads(path.read_text());validate(record,case,seed,args.rounds)
        case["runs"].append({"process":process,"path":str(path.relative_to(out)),"sha256":sha(path),
                             "argv":list(map(str,argv)),"started_utc":began,"ended_utc":now(),"wall_ns":wall})
        write(out/"suite.json",suite);print(f"Measured {case['name']} process {process}",flush=True)
    freeze_gate(suite,binary,benchmark)
    suite["measurement_completed_utc"]=now();suite["status"]="complete";write(out/"suite.json",suite)
    result=summarize(suite,out);print(json.dumps({"status":result["status"],"processes":result["process_count"],"checks":result["complete_output_checks"]}))
if __name__=="__main__":main()
