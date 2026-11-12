[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Compiler = 'cl.exe',
    [string]$Python = 'python',
    [ValidateRange(-1, 63)][int]$LogicalCpu = -1,
    [switch]$PrepareOnly
)

# Native target workflow, supplied as source. Not executed in the Linux release.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'Run this workflow on native Windows, outside WSL.'
}
$projectDirectory = Split-Path -Parent $PSScriptRoot
$experimentDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $experimentDirectory) { throw 'Choose a new OutputDirectory.' }
$compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
$pythonPath = (Get-Command $Python -ErrorAction Stop).Source
$process = [Diagnostics.Process]::GetCurrentProcess()
$originalAffinity = $process.ProcessorAffinity
$hadCompilerVariable = Test-Path Env:CXX
$previousCompilerVariable = $env:CXX
$affinityChanged = $false
New-Item -ItemType Directory -Path $experimentDirectory | Out-Null

function Invoke-CheckedPython([string[]]$Arguments) {
    & $pythonPath @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Python stage failed: $LASTEXITCODE" }
}
function Write-HostRecord([string]$Name) {
    [ordered]@{
        captured_utc = [DateTime]::UtcNow.ToString('o')
        native_windows = $true
        cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, Manufacturer, NumberOfCores, NumberOfLogicalProcessors)
        os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version, BuildNumber, OSArchitecture
        process_affinity_mask = $process.ProcessorAffinity.ToInt64().ToString('X16')
        compiler = $compilerPath
        python = $pythonPath
        note = 'Record power, thermal and competing-workload conditions separately. Processor index does not identify P/E core type.'
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $experimentDirectory $Name) -Encoding UTF8
}
Push-Location $projectDirectory
try {
    $env:CXX = $compilerPath
    if ($LogicalCpu -ge 0) {
        $mask = [Int64]1 -shl $LogicalCpu
        if (($originalAffinity.ToInt64() -band $mask) -eq 0) { throw 'CPU is outside the current process group/affinity.' }
        $process.ProcessorAffinity = [IntPtr]$mask
        $affinityChanged = $true
        if ($process.ProcessorAffinity.ToInt64() -ne $mask) { throw 'Affinity setting did not hold.' }
    }
    Write-HostRecord 'host_before.json'
    $buildDirectory = Join-Path $experimentDirectory 'build'
    & (Join-Path $projectDirectory 'scripts/build.ps1') -Configuration Release -Compiler $compilerPath -BuildDirectory $buildDirectory
    $binary = Join-Path $buildDirectory 'suffice.exe'
    Invoke-CheckedPython @('scripts/test_codegen_v0_9.py', '--compiler', $compilerPath, '--output', (Join-Path $experimentDirectory 'generated_checks'))
    Invoke-CheckedPython @('scripts/reproduce_quotient_v0_9.py', '--output', (Join-Path $experimentDirectory 'state_analysis'))
    $matrixArguments = @('scripts/benchmark_v0_9.py', '--binary', $binary, '--compiler', $compilerPath,
        '--output', (Join-Path $experimentDirectory 'matrix'))
    Invoke-CheckedPython ($matrixArguments + '--prepare-only')
    if (-not $PrepareOnly) {
        Write-HostRecord 'host_measurement.json'
        Invoke-CheckedPython ($matrixArguments + '--resume')
    }
    Write-HostRecord 'host_after.json'
    Write-Host "Evidence is in $experimentDirectory"
}
finally {
    if ($affinityChanged) { $process.ProcessorAffinity = $originalAffinity }
    if ($hadCompilerVariable) { $env:CXX = $previousCompilerVariable }
    else { Remove-Item Env:CXX -ErrorAction SilentlyContinue }
    Pop-Location
}
