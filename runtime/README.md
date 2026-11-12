# Suffice 0.13 — complete-output binary transport

Suffice is a C++20 and Python research runtime for exact predicates and reusable file artifacts over immutable captured generations. Its persistent native catalog preserves generation, query, range, row-order, path and body identity while exposing explicit retained-state limits.

Version 0.13 adds a bounded **SFC1 binary response transport** and a validating Python decoder. The same request can return the same complete consumer-visible output through either JSONL/hex or binary framing. JSONL remains the compatibility default; binary is an explicit option.

Start with [REPORT.md](REPORT.md) for results and interpretation, [the wire contract](docs/binary_protocol_v0_13.md) for exact framing/failure semantics, and [the experiment method](docs/transport_experiment_v0_13.md) for measurement boundaries. [PROJECT_HANDOFF.md](PROJECT_HANDOFF.md) records the completed state and next engineering work.

## What is implemented

- A sixteen-byte, versioned, explicitly little-endian frame header and bounded raw path/body rows; a four-MiB binary payload cap is checked before emission or allocation of advertised payloads.
- Exact-read handling, strict sequence/type/count/range validation, bounded response parsing, and the existing Python `{row, path, data}` output shape.
- Sticky native output failure: a partial write or failed flush terminates the stream instead of appending an error to a partial frame.
- Fatal client handling for malformed/truncated output and callback failures; a completely consumed valid native terminal error remains recoverable.
- Per-request and session request/response byte counters, including framing and terminal metadata.
- The 0.12 Worker-retention and Capture mechanisms, preserved under both transports.

Requests keep the existing bounded TAB/LF command protocol. The historical `emit="hex"` name means complete output; binary mode carries those bodies as raw bytes. `emit="hash"` consumes full output into a diagnostic checksum but returns no bodies. The command-line job runner still prints JSON/hex to its own stdout even when its child connection is binary; use the Python API when measuring direct consumption.

Binary framing removes hexadecimal representation and conversion from the native response path. It is not compression, shared memory or zero-copy. Python still materializes byte strings, and the runtime remains caller-serialized.

## Recorded results

The completed experiment passed **100 fresh native processes and 4,840 complete-output checks**, covering **23,480 rows and 419,571,140 compared body bytes**. It uses one preserved 103-file corpus, twelve logical partitions, five scenarios and ten matched process blocks per scenario. Both transports use the same 0.13 binary, client, cache policy and oracle.

| Scenario | Paired geometric JSONL/Binary service ratio | Blocks favoring Binary | Median reduction in complete-session response bytes |
|---|---:|---:|---:|
| Reuse | 2.13808× | 10/10 | 49.59% |
| Scan pressure | 1.79944× | 10/10 | 49.57% |
| Mixed queries | 1.90233× | 10/10 | 49.45% |
| Computed requests | 2.02757× | 10/10 | 49.59% |
| Empty output | 1.07010× | 4/10 | −2.30% |

Ratios above one favor Binary. **The empty-output case is mixed:** its paired median service ratio is 0.99119, and binary uses about 2.30% more response bytes. Its favorable geometric mean does not establish a consistent latency improvement. The report retains raw-wall comparisons and every observation.

Service time includes startup, twelve Full admissions, native work, complete transfer/materialization, statistics and shutdown, minus explicitly timed oracle/audit intervals. These are descriptive local observations from one shared pinned Linux CPU, not confidence intervals or Windows results. The 4,840 output checks are correctness coverage, not independent timing repetitions. Wire-byte reductions are not measurements of RSS, allocation or physical I/O. Source: [SUMMARY.json](results/transport_v0_13/SUMMARY.json).

## Validation status

Both final Release and ASan/UBSan builds passed ten recorded correctness/CLI gates with stable source inputs. The native wire suite passed nine groups, including injected short-write and flush failure. Final Python framing validation passed 26 tests; producer validation passed 22 tests. Full catalog integration passed for JSONL and Binary under both builds. The additional arbitrary-byte transport integration passed all four transport/population arms under both builds, with 28 complete-output checks per build. Worker-only clearing was also exercised through both transports.

Sanitizer leak detection was disabled; the separately installed codec library was not instrumented. Some suites are included in the build gates, so their counts overlap. Actual receipts, tests and the frozen measurement source distinguish executed checks from design proposals.

## Package navigation

| Location | Purpose |
|---|---|
| `src/`, `include/`, `apps/` | Active C++ computational, payload and catalog implementation |
| `scripts/`, `tests/`, `examples/` | Build/capture/client tools, experiments, regressions and sample predicates |
| `docs/binary_protocol_v0_13.md` | Header layout, limits, output identity and failure lifecycle |
| `docs/transport_experiment_v0_13.md` | Prespecified five-scenario paired experiment |
| `research/` | Primary-source transport analysis/reviews and inherited architecture research |
| `results/transport_v0_13/` | Complete new experiment: original corpus/stores, frozen source, build identity, schedule, process records, raw measurements and summary |
| `build/transport_release/`, `build/transport_sanitize/` | Final build manifests and validation logs; consult the release inventory for packaged binaries |
| `results/client_framing/`, `results/producer/` | Final Python parser and producer results |
| `results/integration_*`, `results/transport_integration_*`, `results/worker_*` | Final native/client integration and worker-control results |
| `baseline/Suffice_0_12_0.zip` | Unchanged complete 0.12 release, including its preserved earlier lineage |
| `baseline/v0_12_*` | Unchanged previous report, README, handoff and validation/inventory snapshots |

Historical 0.12 results and its competition-paper lineage remain separate. Older scripts and documents are retained for continuity; some require older datasets. Use the supported 0.13 commands below for new execution. Build objects, interpreter caches and transient corruption fixtures are reproducible intermediate work rather than prerequisites for using the release.

## Dependencies

Use Python 3.12 or later. Native compilation requires a C++20 compiler with its archiver, Zstandard headers and a linkable native Zstandard library. The direct builder installs and downloads nothing. The recorded Linux configuration uses GCC 14.2 and Python 3.12.14; exact identities are in the manifests.

The Python **`zstandard` package is required for capture, producer tests and native integration tests that create fixtures**. The Python client/framing tests and the portable retained-record audit use the standard library. A fresh transport benchmark reuses included captured stores and needs a passed native build, but does not recapture data or require Python `zstandard` solely for that measurement.

CMake configuration is supplied; the recorded release used the direct Python builder. A Linux executable is not a Windows executable, and a build may still require its platform's native codec runtime library or DLL.

## Linux quickstart

Run from the extracted project root. Every `local_*` output below must be new; choose another name consistently if it already exists. Preserve the packaged evidence.

```bash
python3 scripts/build_payload.py --output build/local_release

python3 tests/test_catalog_framing.py \
  --json results/local_client_framing/RESULT.json

python3 tests/test_artifact_catalog.py \
  --packer build/local_release/suffice \
  --json results/local_producer/RESULT.json

python3 tests/test_transport_integration.py \
  --binary build/local_release/suffice_catalog \
  --packer build/local_release/suffice \
  --output results/local_transport_integration
```

The builder's default gates include the new native wire tests. The transport integration command exercises both response transports and both population strategies, including arbitrary body bytes and streaming callbacks.

Run the broader catalog and Worker checks for each transport:

```bash
python3 tests/test_catalog_integration.py \
  --binary build/local_release/suffice_catalog --packer build/local_release/suffice \
  --transport jsonl --output results/local_integration_jsonl

python3 tests/test_catalog_integration.py \
  --binary build/local_release/suffice_catalog --packer build/local_release/suffice \
  --transport binary --output results/local_integration_binary

python3 tests/test_worker_protocol.py \
  --binary build/local_release/suffice_catalog --evidence-source results/transport_v0_13 \
  --transport jsonl --output results/local_worker_jsonl/RESULT.json

python3 tests/test_worker_protocol.py \
  --binary build/local_release/suffice_catalog --evidence-source results/transport_v0_13 \
  --transport binary --output results/local_worker_binary/RESULT.json
```

For a new sanitized build, use `python3 scripts/build_payload.py --sanitize --output build/local_sanitize`. When separately running integration against that build, set `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1` and `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1` in the command environment and use new result directories. The builder sets those options for its own gates.

## Capture and consume complete output

Create a small captured store from the included examples:

```bash
python3 scripts/artifact_catalog.py \
  --root examples --catalog results/local_examples \
  --packer build/local_release/suffice --expected-current none
```

Keep source files quiescent during capture. Individually checked reads do not establish a filesystem-wide transaction over a changing tree.

Then use the Python API from the project root:

```python
from scripts.catalog_client import CatalogProcess

with CatalogProcess("build/local_release/suffice_catalog", transport="binary") as catalog:
    catalog.open("examples", "results/local_examples", expected_revision=0)
    result, rows = catalog.run(
        "examples",
        "input size 0 0\noutput size > 1024\n",
        cache="populate",
    )
    print(result["selected_rows"], result["output_bytes"], result["response_wire_bytes"])
    for row in rows:
        print(row["row"], row["path"], len(row["data"]))
```

Use `collect=False` and an `on_row` callback to consume rows without retaining a result list. The callback still receives a materialized row and must not issue nested requests on the same client. `ProtocolError` or callback failure closes the owned child; a validated terminal `CatalogError` can leave it usable.

## Fresh measurement and portable audit

After a passed Release build, reuse the included immutable corpus directly:

```bash
python3 scripts/benchmark_transport_v0_13.py run \
  --binary build/local_release/suffice_catalog \
  --evidence-source results/transport_v0_13 \
  --output results/local_transport_v0_13 --repeats 10
```

Run mode requires the Linux CPU-affinity APIs, a new output directory, stable measured sources and a passed adjacent build manifest. Repetitions must be a positive multiple of ten. No original absolute workspace path is required. The runner shares a pinned CPU with the native child; it does not reserve exclusive hardware or flush caches. New timings are new observations on the current machine.

The following audit needs only Python's standard library and invokes no native executable:

```bash
python3 scripts/benchmark_transport_v0_13.py audit \
  --output results/transport_v0_13
```

It checks the packaged identities, original-body descriptors, schedule, LRU/resource accounting, row-wire formula, cumulative wire totals and paired arithmetic. It does not replay historical native outputs or reproduce historical timing. The original run compared every returned body byte. To audit the fresh experiment instead, substitute `results/local_transport_v0_13`.

## Windows 11 target

[scripts/run_windows_v0_13.ps1](scripts/run_windows_v0_13.ps1) is a supplied **unexecuted Windows validation workflow**. It uses the builder's existing MSVC path, requires an x64 developer environment plus matching native Zstandard include/library paths, and runs framing, producer, both catalog transports, arbitrary-byte transport and Worker checks. Pass the required `-ZstdInclude` and `-ZstdLibrary` paths from the installed dependency. Use `-Output` and `-EvidenceOutput` to choose fresh destinations.

The script does not establish MSVC compatibility merely by existing. No native Windows build, transport result, latency, RSS or disk measurement was produced for this Linux release. The timing harness remains Linux-specific; portable audit is a separate path. Native Windows execution and completion-aware liveness/RSS work are the next platform gates.

## License and research boundary

The project retains its [MIT license](LICENSE). Dependencies are separately obtained under their own licenses. The current system assumes trusted local storage, coherent capture and serialized callers. It does not yet provide request deadlines, concurrent scheduling, whole-process memory bounds, cross-generation dependency witnesses or model-level task-success results. See the handoff for how to extend these contracts without weakening existing evidence.
