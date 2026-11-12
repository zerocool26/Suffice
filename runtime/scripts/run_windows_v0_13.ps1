# Supplied workflow only: not executed for the Linux 0.13 release.
# Run from an x64 MSVC developer environment with native Zstandard installed.
param(
    [string]$Python = "python",
    [string]$Compiler = "cl.exe",
    [Parameter(Mandatory = $true)][string]$ZstdInclude,
    [Parameter(Mandatory = $true)][string]$ZstdLibrary,
    [string]$Output = "build/windows_v0_13",
    [string]$EvidenceOutput = "results/windows_v0_13",
    [string]$EvidenceSource = "results/transport_v0_13"
)

$ErrorActionPreference = "Stop"
$SufficeProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $SufficeProjectRoot
try {
    if ($env:OS -ne "Windows_NT") { throw "This workflow is for native Windows, not a Linux timing run." }
    if (Test-Path $Output) { throw "Use a new Output directory; existing builds are preserved." }
    if (Test-Path $EvidenceOutput) { throw "Use a new EvidenceOutput directory; existing receipts are preserved." }
    if (-not (Test-Path $ZstdInclude -PathType Container)) { throw "ZstdInclude must name an existing header directory." }
    if (-not (Test-Path (Join-Path $ZstdInclude "zstd.h") -PathType Leaf)) { throw "zstd.h was not found under ZstdInclude." }
    if (-not (Test-Path $ZstdLibrary -PathType Leaf)) { throw "ZstdLibrary must name an existing matching native library." }
    if (-not (Test-Path (Join-Path $EvidenceSource "suite.json") -PathType Leaf)) { throw "Use the included 0.13 evidence directory for EvidenceSource." }

    & $Python -c "import sys; import zstandard; assert sys.version_info >= (3, 12), 'Python 3.12 or later is required'"
    if ($LASTEXITCODE -ne 0) { throw "Python 3.12+ and the zstandard package are required for this workflow." }

    & $Python scripts/build_payload.py --compiler $Compiler --zstd-include $ZstdInclude --zstd-library $ZstdLibrary --output $Output
    if ($LASTEXITCODE -ne 0) { throw "Native build or correctness gate failed. Inspect the new build manifest and logs." }
    $SufficePacker = Join-Path $Output "suffice.exe"
    $SufficeCatalog = Join-Path $Output "suffice_catalog.exe"

    & $Python tests/test_catalog_framing.py --json (Join-Path $EvidenceOutput "client_framing.json")
    if ($LASTEXITCODE -ne 0) { throw "Client framing validation failed." }
    & $Python tests/test_artifact_catalog.py --packer $SufficePacker --json (Join-Path $EvidenceOutput "producer.json")
    if ($LASTEXITCODE -ne 0) { throw "Producer validation failed." }

    foreach ($SufficeTransport in @("jsonl", "binary")) {
        & $Python tests/test_catalog_integration.py --binary $SufficeCatalog --packer $SufficePacker --transport $SufficeTransport --output (Join-Path $EvidenceOutput ("integration_" + $SufficeTransport))
        if ($LASTEXITCODE -ne 0) { throw "Full catalog integration failed for $SufficeTransport." }
        & $Python tests/test_worker_protocol.py --binary $SufficeCatalog --evidence-source $EvidenceSource --transport $SufficeTransport --output (Join-Path $EvidenceOutput ("worker_" + $SufficeTransport + ".json"))
        if ($LASTEXITCODE -ne 0) { throw "Worker protocol validation failed for $SufficeTransport." }
    }

    & $Python tests/test_transport_integration.py --binary $SufficeCatalog --packer $SufficePacker --output (Join-Path $EvidenceOutput "transport_integration")
    if ($LASTEXITCODE -ne 0) { throw "Arbitrary-byte transport integration failed." }

    Write-Output "All requested commands completed successfully. Read the newly generated receipts for actual Windows outcomes."
    Write-Output "This workflow does not measure Windows latency, RSS or disk use. The Linux timing harness is separate."
}
finally {
    Pop-Location
}
