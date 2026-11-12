param(
    [string]$Python = "python",
    [string]$Compiler = "cl.exe",
    [Parameter(Mandatory = $true)][string]$ZstdInclude,
    [Parameter(Mandatory = $true)][string]$ZstdLibrary,
    [string]$Output = "build/windows_v0_12",
    [string]$EvidenceOutput = "results/windows_v0_12"
)

$ErrorActionPreference = "Stop"
$SufficeProjectRoot = Split-Path -Parent $PSScriptRoot
Push-Location $SufficeProjectRoot
try {
    if (Test-Path $Output) { throw "Use a new Output directory; existing measurements are preserved." }
    if (Test-Path $EvidenceOutput) { throw "Use a new EvidenceOutput directory." }
    & $Python scripts/build_payload.py --compiler $Compiler --zstd-include $ZstdInclude --zstd-library $ZstdLibrary --output $Output
    if ($LASTEXITCODE -ne 0) { throw "Native build or correctness gate failed." }
    $SufficePacker = Join-Path $Output "suffice.exe"
    $SufficeCatalog = Join-Path $Output "suffice_catalog.exe"
    & $Python tests/test_artifact_catalog.py --packer $SufficePacker --json (Join-Path $EvidenceOutput "producer.json")
    if ($LASTEXITCODE -ne 0) { throw "Producer validation failed." }
    & $Python tests/test_catalog_integration.py --binary $SufficeCatalog --packer $SufficePacker --output (Join-Path $EvidenceOutput "integration")
    if ($LASTEXITCODE -ne 0) { throw "Full integration validation failed." }
    & $Python tests/test_worker_protocol.py --binary $SufficeCatalog --evidence-source results/retention_v0_12 --output (Join-Path $EvidenceOutput "worker_protocol.json")
    if ($LASTEXITCODE -ne 0) { throw "Worker protocol validation failed." }
    Write-Output "Native checks completed. Read the generated receipts before making platform claims."
    Write-Output "The Linux affinity-controlled timing harness is not a Windows performance measurement."
}
finally {
    Pop-Location
}
