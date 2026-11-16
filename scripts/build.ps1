<#
.SYNOPSIS
  Configure, build and test MiniExchange on Windows with a modern MinGW-w64 GCC.

.DESCRIPTION
  Puts WinLibs GCC (installed with `winget install BrechtSanders.WinLibs.POSIX.UCRT`)
  ahead of any older MinGW on PATH, prefers the pip-installed CMake (`pip install
  cmake ninja`, whose TLS stack can download GoogleTest), then builds with Ninja.

.EXAMPLE
  ./scripts/build.ps1                     # Release, C++20, run tests
.EXAMPLE
  ./scripts/build.ps1 -Config Debug -Std 17
.EXAMPLE
  ./scripts/build.ps1 -Bench              # also run the benchmarks
#>
param(
  [ValidateSet('Release', 'Debug', 'RelWithDebInfo')] [string]$Config = 'Release',
  [ValidateSet('17', '20')] [string]$Std = '20',
  [switch]$SkipTests,
  [switch]$Bench
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$packages = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'
$winlibs = Get-ChildItem $packages -Directory -Filter 'BrechtSanders.WinLibs*' -ErrorAction SilentlyContinue |
  ForEach-Object { Join-Path $_.FullName 'mingw64\bin' } |
  Where-Object { Test-Path (Join-Path $_ 'g++.exe') } |
  Select-Object -First 1
if ($winlibs) { $env:PATH = "$winlibs;$env:PATH" }

try {
  $pyScripts = & python -c "import sysconfig; print(sysconfig.get_path('scripts', 'nt_user'))"
  if ($pyScripts -and (Test-Path (Join-Path $pyScripts 'cmake.exe'))) { $env:PATH = "$pyScripts;$env:PATH" }
} catch { }

$gxx = (Get-Command g++ -ErrorAction Stop).Source
$version = & $gxx -dumpfullversion
if ([int]($version.Split('.')[0]) -lt 9) {
  throw "g++ $version at $gxx is too old (need GCC 9+ for C++17/20). Install WinLibs: winget install BrechtSanders.WinLibs.POSIX.UCRT"
}
Write-Host "Compiler: $gxx (GCC $version)"
Write-Host "CMake:    $((Get-Command cmake).Source)"

$build = Join-Path $root ("out\{0}-cpp{1}" -f $Config.ToLower(), $Std)
cmake -S $root -B $build -G Ninja "-DCMAKE_BUILD_TYPE=$Config" "-DMX_CXX_STANDARD=$Std" "-DCMAKE_CXX_COMPILER=$gxx"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
cmake --build $build --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $SkipTests) {
  ctest --test-dir $build --output-on-failure --parallel 8
  if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
if ($Bench) {
  & (Join-Path $build 'bin\miniexchange_bench.exe')
}
Write-Host "`nBinaries: $(Join-Path $build 'bin')"
