[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [string]$Compiler = 'cl.exe',
    [string]$Python = 'python',
    [ValidateRange(-1, 63)][int]$LogicalCpu = -1,
    [switch]$PrepareOnly,
    [switch]$KeepData
)

# Native-Windows reproduction entry point. Supplied for validation on the
# target host; this script was not executed in the Linux release environment.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
    throw 'Use native Windows PowerShell/PowerShell, not WSL, for this experiment.'
}
$projectDirectory = Split-Path -Parent $PSScriptRoot
$experimentDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $experimentDirectory) {
    throw 'OutputDirectory must be new; existing experiment evidence is never replaced.'
}
$compilerPath = (Get-Command $Compiler -ErrorAction Stop).Source
$pythonPath = (Get-Command $Python -ErrorAction Stop).Source
$baselineDirectory = Join-Path $projectDirectory 'baseline/v0_7'
$currentBuild = Join-Path $experimentDirectory 'build_v08'
$baselineBuild = Join-Path $experimentDirectory 'build_v07'
$matrixDirectory = Join-Path $experimentDirectory 'matrix'
$summaryDirectory = Join-Path $experimentDirectory 'summary'
$hadCompilerVariable = Test-Path Env:CXX
$previousCompilerVariable = $env:CXX
$process = [Diagnostics.Process]::GetCurrentProcess()
$originalAffinity = $process.ProcessorAffinity
$affinityChanged = $false
New-Item -ItemType Directory -Path $experimentDirectory | Out-Null

function Invoke-CheckedPython([string[]]$Arguments) {
    & $pythonPath @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Python stage failed with exit code $LASTEXITCODE" }
}

function Write-HostRecord([string]$Name) {
    $cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, Manufacturer,
        NumberOfCores, NumberOfLogicalProcessors, MaxClockSpeed)
    $os = Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version,
        BuildNumber, OSArchitecture
    [ordered]@{
        captured_utc = [DateTime]::UtcNow.ToString('o')
        native_windows = $true
        cpu = $cpu
        operating_system = $os
        compiler = $compilerPath
        python = $pythonPath
        requested_logical_cpu_in_current_group = $LogicalCpu
        process_affinity_mask = $process.ProcessorAffinity.ToInt64().ToString('X16')
        stopwatch_is_high_resolution = [Diagnostics.Stopwatch]::IsHighResolution
        stopwatch_frequency_hz = [Diagnostics.Stopwatch]::Frequency
        timing_scope = 'Native benchmark uses std::chrono::steady_clock; Stopwatch frequency is complementary Windows timer metadata.'
        note = 'No CPU model/core-class inference is made from logical processor number. Record power/thermal/concurrent workload conditions separately.'
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $experimentDirectory $Name) -Encoding UTF8
}

Push-Location $projectDirectory
try {
    $env:CXX = $compilerPath
    Write-HostRecord 'host_before.json'
    & (Join-Path $projectDirectory 'scripts/build.ps1') -Configuration Release -Compiler $compilerPath -BuildDirectory $currentBuild
    & (Join-Path $baselineDirectory 'scripts/build.ps1') -Configuration Release -Compiler $compilerPath -BuildDirectory $baselineBuild -NoTests
    $currentBinary = Join-Path $currentBuild 'suffice.exe'
    $baselineBinary = Join-Path $baselineBuild 'suffice.exe'
    [ordered]@{
        current_version = (& $currentBinary --version | Out-String).Trim()
        baseline_version = (& $baselineBinary --version | Out-String).Trim()
        current_sha256 = (Get-FileHash -LiteralPath $currentBinary -Algorithm SHA256).Hash.ToLowerInvariant()
        baseline_sha256 = (Get-FileHash -LiteralPath $baselineBinary -Algorithm SHA256).Hash.ToLowerInvariant()
        current_build_script = 'scripts/build.ps1'
        baseline_build_script = 'baseline/v0_7/scripts/build.ps1'
        configuration = 'Release, portable target, same compiler; baseline emitter built without the historical test suite'
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $experimentDirectory 'native_emitters.json') -Encoding UTF8

    Invoke-CheckedPython @('scripts/test_cli_v0_8.py', '--binary', $currentBinary,
        '--output', (Join-Path $experimentDirectory 'cli_checks'))
    Invoke-CheckedPython @('scripts/test_codegen_v0_8.py', '--compiler', $compilerPath,
        '--output', (Join-Path $experimentDirectory 'generated_checks'))
    Invoke-CheckedPython @('scripts/test_portfolio_v0_8.py', '--compiler', $compilerPath,
        '--output', (Join-Path $experimentDirectory 'portfolio_checks.json'))

    $matrixArguments = @('scripts/reproduce_v0_8.py', '--binary', $currentBinary,
        '--baseline-binary', $baselineBinary, '--output', $matrixDirectory, '--pin-cpu', 'none')
    if ($KeepData) { $matrixArguments += '--keep-data' }
    Invoke-CheckedPython ($matrixArguments + '--prepare-only')
    if (-not $PrepareOnly) {
        # Pin only the measurement phase. Windows children inherit this mask.
        # This entry point addresses one current processor group (at most64 bits).
        if ($LogicalCpu -ge 0) {
            $mask = [Int64]1 -shl $LogicalCpu
            if (($originalAffinity.ToInt64() -band $mask) -eq 0) {
                throw 'Requested CPU is outside this process affinity mask/current processor group.'
            }
            $process.ProcessorAffinity = [IntPtr]$mask
            $affinityChanged = $true
            if ($process.ProcessorAffinity.ToInt64() -ne $mask) { throw 'Affinity verification failed.' }
        }
        Write-HostRecord 'host_measurement.json'
        Invoke-CheckedPython ($matrixArguments + '--resume')
        Invoke-CheckedPython @('scripts/summarize_v0_8.py', '--matrix', $matrixDirectory,
            '--output', $summaryDirectory)
    }
    Write-HostRecord 'host_after.json'
    Write-Host "Experiment evidence: $experimentDirectory"
}
finally {
    if ($affinityChanged) { $process.ProcessorAffinity = $originalAffinity }
    if ($hadCompilerVariable) { $env:CXX = $previousCompilerVariable }
    else { Remove-Item Env:CXX -ErrorAction SilentlyContinue }
    Pop-Location
}
