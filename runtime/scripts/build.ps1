[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'Sanitize')]
    [string]$Configuration = 'Release',
    [string]$Compiler = '',
    [string]$BuildDirectory = '',
    [switch]$Native,
    [switch]$NoTests,
    [switch]$NoRun,
    [switch]$Help
)

$ErrorActionPreference = 'Stop'
if ($Help) {
    Write-Host @'
Usage: .\scripts\build.ps1 [-Configuration Release|Debug|Sanitize]
                           [-Compiler path] [-BuildDirectory path]
                           [-Native] [-NoTests] [-NoRun] [-Help]

Builds Suffice and, by default, builds and runs the correctness tests.
Use Developer PowerShell for Visual Studio with C++ build tools, or provide
a GCC/Clang executable. No package installation or downloads are performed.
The combined sanitizer configuration and native tuning require GCC/Clang.
'@
    return
}
$projectDirectory = Split-Path -Parent $PSScriptRoot
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $projectDirectory 'build' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
New-Item -ItemType Directory -Force -Path $BuildDirectory | Out-Null

if (-not $Compiler -and $env:CXX) { $Compiler = $env:CXX }
if (-not $Compiler) {
    foreach ($candidate in @('cl.exe', 'clang++.exe', 'g++.exe')) {
        $found = Get-Command $candidate -ErrorAction SilentlyContinue
        if ($found) { $Compiler = $found.Source; break }
    }
}
if (-not $Compiler) {
    throw 'No C++20 compiler found. Open Developer PowerShell for Visual Studio with C++ build tools, or supply -Compiler with a Clang/GCC executable.'
}
$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$compilerName = [IO.Path]::GetFileName($compilerPath).ToLowerInvariant()
$isMsvc = $compilerName -in @('cl.exe', 'cl', 'clang-cl.exe', 'clang-cl')
if ($isMsvc -and $Configuration -eq 'Sanitize') {
    throw 'The combined AddressSanitizer/UndefinedBehaviorSanitizer configuration requires GCC or Clang. MSVC sanitizer coverage is not claimed.'
}
if ($isMsvc -and $Native) {
    throw '-Native is a GCC/Clang option. Use a portable MSVC build or supply explicit architecture flags through your own CMake configuration.'
}

$includeDirectory = Join-Path $projectDirectory 'include'
$coreSources = @('src/ir.cpp', 'src/storage.cpp', 'src/preimage.cpp', 'src/encoded_ranges.cpp', 'src/runtime.cpp', 'src/vector.cpp', 'src/predicate.cpp', 'src/adaptive.cpp', 'src/wordcode.cpp', 'src/frontier.cpp', 'src/residual.cpp', 'src/boolean_proof.cpp', 'src/frontier_runtime.cpp', 'src/codegen.cpp', 'src/portfolio_codegen.cpp') |
    ForEach-Object { Join-Path $projectDirectory $_ }
$mainSource = Join-Path $projectDirectory 'src/main.cpp'
$testSources = @('tests/main.cpp', 'tests/test_ir.cpp', 'tests/test_storage.cpp', 'tests/test_preimage.cpp', 'tests/test_encoded_ranges.cpp', 'tests/test_runtime.cpp', 'tests/test_vector.cpp', 'tests/test_predicate.cpp', 'tests/test_adaptive.cpp', 'tests/test_decode.cpp', 'tests/test_ranges.cpp', 'tests/test_wordcode.cpp', 'tests/test_frontier.cpp', 'tests/test_residual.cpp', 'tests/test_boolean_proof.cpp', 'tests/test_frontier_runtime.cpp', 'tests/test_codegen.cpp', 'tests/test_portfolio.cpp') |
    ForEach-Object { Join-Path $projectDirectory $_ }

function Invoke-Compiler([string[]]$Arguments) {
    & $compilerPath @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Compiler failed with exit code $LASTEXITCODE" }
}

if ($isMsvc) {
    $common = @('/nologo', '/std:c++20', '/EHsc', '/W4', '/permissive-', '/Zc:__cplusplus', "/I$includeDirectory")
    if ($Configuration -eq 'Release') { $common += @('/O2', '/DNDEBUG', '/MT') }
    else { $common += @('/Od', '/Zi', '/MTd') }
    $objectDirectory = Join-Path $BuildDirectory 'obj_cli'
    New-Item -ItemType Directory -Force -Path $objectDirectory | Out-Null
    Push-Location $objectDirectory
    try {
        Invoke-Compiler ($common + $coreSources + @($mainSource, "/Fe:$(Join-Path $BuildDirectory 'suffice.exe')"))
    }
    finally { Pop-Location }
    if (-not $NoTests) {
        $testObjectDirectory = Join-Path $BuildDirectory 'obj_tests'
        New-Item -ItemType Directory -Force -Path $testObjectDirectory | Out-Null
        Push-Location $testObjectDirectory
        try {
            Invoke-Compiler ($common + @('/UNDEBUG') + $coreSources + $testSources + @("/Fe:$(Join-Path $BuildDirectory 'suffice_tests.exe')"))
        }
        finally { Pop-Location }
    }
}
else {
    $common = @('-std=c++20', '-pthread', '-Wall', '-Wextra', '-Wpedantic', "-I$includeDirectory")
    switch ($Configuration) {
        'Release' { $common += @('-O3', '-DNDEBUG') }
        'Debug' { $common += @('-O0', '-g') }
        'Sanitize' { $common += @('-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer') }
    }
    if ($Native) { $common += '-march=native' }
    Invoke-Compiler ($common + $coreSources + @($mainSource, '-o', (Join-Path $BuildDirectory 'suffice.exe')))
    if (-not $NoTests) {
        Invoke-Compiler ($common + @('-UNDEBUG') + $coreSources + $testSources + @('-o', (Join-Path $BuildDirectory 'suffice_tests.exe')))
    }
}

if (-not $NoTests -and -not $NoRun) {
    & (Join-Path $BuildDirectory 'suffice_tests.exe')
    if ($LASTEXITCODE -ne 0) { throw "Correctness tests failed with exit code $LASTEXITCODE" }
}
Write-Host "Executable: $(Join-Path $BuildDirectory 'suffice.exe')"
