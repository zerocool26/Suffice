# Suffice 0.12 — exact artifact reuse with independent worker retention

Suffice is a C++20/Python research runtime for selecting and returning exact file artifacts from immutable captured generations. It combines verified feature/payload storage, prepared integer predicates, bounded retained state, complete-result reuse and a persistent native service. Its current role is a local repository-tool backend. It is not a general compiler, operating system, concurrent scheduler, or replacement for Cargo.

**Start with [REPORT.md](REPORT.md)** for the measured results and [the architecture](docs/architecture_v0_12.md) for the engineering model, proof obligations and next research directions.

## What this release changes

Two implemented changes address costs identified in the preserved 0.11 experiment:

1. **Independent worker retention.** A prepared Query can survive while its mutable Worker is evicted. A later computation recreates the Worker without recompiling the Query. Worker bytes and worker entries have separate limits; complete-result replay acquires no worker. The defaults are 16 MiB and 32 retained workers.
2. **Capture during useful output.** The default Populate strategy computes the exact selection, checks its decoded lengths, privately allocates a bounded cache candidate and captures bodies during the normal decode/delivery stream. It admits the candidate only after successful output. The selectable Recompute control preserves the older normal run plus two cache-construction decode passes.

Both preserve the ordered generation-local row IDs, relative paths and complete original bodies. Result identity still includes the immutable generation owner, exact program-source bytes and requested range. Optional cache allocation failure permits uncached delivery; a throwing sink publishes no partial candidate. Old leases still return the old generation and retain no new internal plans or workers.

Worker limits hold at completed API boundaries, including exceptional exits. They bound reported retained worker components, **not peak RSS or transient construction memory**. The Worker is still eagerly prepared; lazy backends, learned admission, binary transport, cross-generation dependency witnesses and a reservation scheduler remain proposals.

See [the implemented worker/capture contract](docs/worker_retention_v0_12.md). The original 0.11 paper is preserved unchanged at [baseline/Suffice_0_11_Paper.md](baseline/Suffice_0_11_Paper.md); its historical measurements have not been relabeled as 0.12 results.

## Recorded validation and experiment

The included final Release and ASan/UBSan builds each passed nine recorded gates with unchanged compiled inputs. Producer validation passed 22 cases. Both integration builds passed their seven groups, and the worker protocol check exercised both population strategies. Sanitizer leak detection was disabled. Counts overlap and are not a count of independent experiments.

The new timing experiment completed **112 native processes and 5,440 full-output checks**, comparing **32,064 rows and 578,187,424 original bytes**. It uses one preserved 103-file corpus partitioned into twelve logical projects, three length predicates and four scenarios. Every configuration is executed by the same 0.12 binary; comparisons do not combine new timings with historical 0.11 timings.

| Population comparison, 16 MiB workers | Paired geometric Recompute/Capture service ratio | Repetitions favoring Capture |
|---|---:|---:|
| Reuse | 1.02710 | 4 / 8 |
| Scan pressure | 1.17237 | 8 / 8 |
| Mixed queries | 1.08034 | 6 / 8 |

Above one favors Capture. These are descriptive observations from one shared Linux configuration, not confidence intervals or a universal speedup. The separate 256 KiB worker-budget arms measure retention and recreation tradeoffs; the full report presents memory-component and latency outcomes together. Source: [SUMMARY.json](results/retention_v0_12/SUMMARY.json).

## Package guide

| Location | Contents |
|---|---|
| [REPORT.md](REPORT.md) | Release findings, measured comparisons and remaining limitations |
| [docs/architecture_v0_12.md](docs/architecture_v0_12.md) | Broader validity, ownership, economics and scheduling model; implemented/proposed boundaries |
| [docs/worker_retention_v0_12.md](docs/worker_retention_v0_12.md) | New APIs, limits, lifetime rules, telemetry and failure behavior |
| [docs/retention_experiment_v0_12.md](docs/retention_experiment_v0_12.md) | Scenarios, balanced ordering, oracle, timing and audit methodology |
| `src/`, `include/`, `apps/` | Active C++ core, payload and catalog implementation |
| `scripts/`, `tests/`, `examples/` | Build/capture/client/reproduction tools, regressions and predicate examples |
| `research/` | Primary-source catalogs, mathematical reasoning, cache-policy analysis and source reviews |
| `results/retention_v0_12/` | Complete new evidence: original inputs, captured stores, frozen measured sources, 112 process records, build identity, run and summary |
| `build/retention_release/` | Recorded build manifest/logs and the two packaged Linux Release executables, `suffice` and `suffice_catalog` |
| `build/retention_sanitize/` | Recorded sanitized manifest/logs; sanitizer binaries are omitted |
| `results/producer/`, `results/integration_release/`, `results/integration_sanitize/`, `results/worker_protocol/` | Final validation result records; temporary integration fixtures are omitted |
| `baseline/` | Unchanged 0.11 source ZIP, source inventory, paper and selected historical validation records |

Object files, code-generation caches, sanitizer executables, transient integration fixtures and third-party paper PDFs are not part of the release package. Some inherited historical scripts refer to older datasets that are not included; the supported 0.12 reproduction commands are below.

## Dependencies

Use Python 3.12 or later, a C++20 compiler with its static-library archiver, native Zstandard headers and a linkable Zstandard library. Capture and producer/integration validation also require the Python `zstandard` package. These dependencies are **not vendored**. The direct builder installs and downloads nothing; configure the compiler and dependencies first.

The measured Linux build used GCC 14.2 and Python 3.12.14. Exact compiler, codec, environment and file identities are retained in the build and measurement records. The supplied executables are Linux builds with host/runtime dependencies, not portable Windows binaries. Rebuild on the target machine.

The inherited payload application includes a dynamically loaded SQLite comparison adapter; the catalog is not a SQLite database. CMake configuration is supplied, while the recorded release gates used the direct Python builder.

## Linux build and correctness gates

Run these commands from the extracted project root. All `local_*` destinations below must be new; choose another name and update later commands if one already exists. Do not overwrite the packaged evidence.

Build the native applications and run the builder's default correctness/smoke gates:

```bash
python3 scripts/build_payload.py --output build/local_release
```

Run the producer, complete native/Python integration, and worker-control protocol checks:

```bash
python3 tests/test_artifact_catalog.py \
  --packer build/local_release/suffice \
  --json results/local_producer/RESULT.json

python3 tests/test_catalog_integration.py \
  --binary build/local_release/suffice_catalog \
  --packer build/local_release/suffice \
  --output results/local_integration_release

python3 tests/test_worker_protocol.py \
  --binary build/local_release/suffice_catalog \
  --evidence-source results/retention_v0_12 \
  --output results/local_worker_protocol/RESULT.json
```

For a new sanitized build and integration check:

```bash
python3 scripts/build_payload.py --sanitize --output build/local_sanitize

ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
python3 tests/test_catalog_integration.py \
  --binary build/local_sanitize/suffice_catalog \
  --packer build/local_sanitize/suffice \
  --output results/local_integration_sanitize
```

The builder sets these sanitizer options for its own gates. The explicit environment above applies them to the separately launched integration check. These commands produce new validation records; they do not reproduce a historical executable hash merely by using the same source.

## Capture a tree and use the service

For a small new capture of the supplied examples:

```bash
python3 scripts/artifact_catalog.py \
  --root examples \
  --catalog results/local_example_store \
  --packer build/local_release/suffice \
  --expected-current none
```

Keep the source quiescent during capture. The producer validates individually read files and reconciles membership; it does not establish a filesystem-wide transaction over a changing source.

The service can then be launched with explicit retention and population controls:

```bash
build/local_release/suffice_catalog serve \
  --max-worker-bytes 262144 \
  --max-worker-entries 32 \
  --population capture
```

It uses a bounded tab-separated stdin protocol with JSON-line responses and no network listener. Use the Python `CatalogProcess` API in [scripts/catalog_client.py](scripts/catalog_client.py) for normal integration rather than manually encoding text fields. The inherited [protocol documentation](docs/catalog_protocol_v0_11.md) describes requests; the [0.12 contract](docs/worker_retention_v0_12.md) adds worker controls and `CLEAR workers`.

## Run a fresh controlled experiment

Use the included completed evidence as the input source. No original absolute 0.11 workspace path or recapture is required:

```bash
python3 scripts/benchmark_retention_v0_12.py run \
  --binary build/local_release/suffice_catalog \
  --evidence-source results/retention_v0_12 \
  --output results/local_retention_v0_12 \
  --repeats 8
```

The output directory must not exist. The harness copies and verifies the preserved inputs, freezes the current measured sources, checks the adjacent passed nonsanitized build manifest, executes all arms and writes a recomputed summary. Repeats must be a positive multiple of four and at least four.

Run mode requires Linux CPU-affinity APIs and a permitted logical CPU. It pins the runner and verifies the inherited child affinity, but does not reserve that CPU exclusively or flush filesystem caches. Keep compiled source inputs and binaries unchanged throughout the run. New timings are new observations on the current host.

## Audit retained evidence without native execution

The following uses Python's standard library only. It needs neither a compiler nor Zstandard installation, invokes no native program, and reads the included evidence rather than historical external paths:

```bash
python3 scripts/benchmark_retention_v0_12.py audit \
  --output results/retention_v0_12
```

To audit the fresh run created above:

```bash
python3 scripts/benchmark_retention_v0_12.py audit \
  --output results/local_retention_v0_12
```

Audit recomputes identities, saved output descriptors, resource and LRU accounting, scheduling, phase arithmetic and paired summaries. The original measurement compared every complete body byte; this later audit does not re-execute historical native output, decode payload frames or reproduce historical timings. A saved summary must match recomputation exactly. `summarize` can write a missing summary, but does not replace an existing one.

## Native Windows status

[scripts/run_windows_v0_12.ps1](scripts/run_windows_v0_12.ps1) supplies a native build/producer/integration/worker-protocol workflow. Run it from an x64 MSVC developer environment with `-ZstdInclude` and `-ZstdLibrary` pointing to an existing matching Zstandard installation, and new output directories. The [Windows setup notes](docs/windows_catalog_v0_11.md) explain the inherited toolchain requirements.

**The 0.12 Windows runner was supplied but not executed in this release.** The included Linux correctness/timing records do not establish native Windows latency, RSS, filesystem durability or compiler compatibility. The affinity-controlled timing harness is currently Linux-specific; its portable audit path is separate.

## License and research status

The project retains its [MIT license](LICENSE). Separately obtained dependencies retain their own licenses. This is a research implementation with explicit trusted-local-storage, serialized-caller and coherent-capture assumptions. AI assistance was used in implementation, analysis, research discovery, review and documentation; the supplied execution receipts distinguish actual tests and measurements from proposals.
