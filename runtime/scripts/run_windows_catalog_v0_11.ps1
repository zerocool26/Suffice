param(
    [Parameter(Mandatory=$true)][string]$ZstdInclude,
    [Parameter(Mandatory=$true)][string]$ZstdLibrary,
    [string]$ZstdDllDirectory = "",
    [string]$ReleaseDirectory = "build\catalog_windows",
    [string]$PythonExe = "python"
)
$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
$releasePath = if ([IO.Path]::IsPathRooted($ReleaseDirectory)) { $ReleaseDirectory } else { Join-Path $projectRoot $ReleaseDirectory }
$releasePath = [IO.Path]::GetFullPath($releasePath)
if (Test-Path -LiteralPath $releasePath) { throw "Use a fresh ReleaseDirectory; existing evidence will not be overwritten." }
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) { throw "Run in an x64 Visual Studio Developer PowerShell with cl.exe available." }
if (-not (Test-Path -LiteralPath (Join-Path $ZstdInclude "zstd.h"))) { throw "ZstdInclude must contain zstd.h." }
if (-not (Test-Path -LiteralPath $ZstdLibrary -PathType Leaf)) { throw "ZstdLibrary must name an existing x64 .lib file." }
$savedTaskPath = $env:PATH
Push-Location $projectRoot
try {
    if ($ZstdDllDirectory) { $env:PATH = $ZstdDllDirectory + ";" + $env:PATH }
    & $PythonExe -c "import sys,zstandard; print(sys.version); print('Python zstandard',zstandard.__version__)"
    if ($LASTEXITCODE -ne 0) { throw "Python 3.12+ and its zstandard package must be installed." }
    & $PythonExe scripts/build_payload.py --compiler cl.exe --output $releasePath --zstd-include $ZstdInclude --zstd-library $ZstdLibrary
    if ($LASTEXITCODE -ne 0) { throw "Native build or correctness checks failed; inspect the build manifest/logs." }
    $validationPath = Join-Path $releasePath "validation"
    New-Item -ItemType Directory -Path $validationPath | Out-Null
    $packerPath = Join-Path $releasePath "suffice.exe"
    $catalogPath = Join-Path $releasePath "suffice_catalog.exe"
    & $PythonExe tests/test_artifact_catalog.py --packer $packerPath --json (Join-Path $validationPath "producer.json")
    if ($LASTEXITCODE -ne 0) { throw "Artifact capture validation failed." }
    & $PythonExe tests/test_catalog_integration.py --binary $catalogPath --packer $packerPath --output (Join-Path $validationPath "integration")
    if ($LASTEXITCODE -ne 0) { throw "Persistent catalog integration validation failed." }
    Write-Host "Completed native build and catalog checks. Evidence: $releasePath"
} finally {
    $env:PATH = $savedTaskPath
    Pop-Location
}
