param(
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [string]$QtRoot = $env:QTDIR,
    [string]$LibsRoot = (Join-Path $PSScriptRoot 'libs'),
    [switch]$SkipTests,
    [switch]$NoDeploy
)
$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($QtRoot)) {
    throw 'Pass -QtRoot <Qt installation> or set QTDIR. Example: .\build.ps1 -QtRoot E:\Qt\6.4.3\msvc2019_64'
}
if (-not (Test-Path -LiteralPath (Join-Path $QtRoot 'bin\qmake.exe'))) {
    throw "Qt installation not found: $QtRoot"
}
if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $vsWhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vsWhere)) {
        throw 'Run from Developer PowerShell for Visual Studio, with the C++ tools installed.'
    }
    $vsPath = & $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ([string]::IsNullOrWhiteSpace($vsPath)) {
        throw 'MSVC C++ tools were not found.'
    }
    Import-Module (Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64'
}
$env:VSLANG = '1033'
$env:PATH = (Join-Path $QtRoot 'bin') + ';' + $env:PATH
$buildDirectory = Join-Path $PSScriptRoot ('out\build\' + $Configuration.ToLowerInvariant())
& cmake -S $PSScriptRoot -B $buildDirectory -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" "-DORKESTRATOR_QT_ROOT=$QtRoot" "-DORKESTRATOR_LIBS_ROOT=$LibsRoot"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed.' }
& cmake --build $buildDirectory --parallel
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if (-not $SkipTests) {
    & ctest --test-dir $buildDirectory --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
if (-not $NoDeploy) {
    $deployDirectory = Join-Path $PSScriptRoot ("dist\" + $Configuration)
    & cmake --install $buildDirectory --prefix $deployDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Deployment failed.' }
    Write-Host "Application: $deployDirectory\ORKestrator.exe"
}