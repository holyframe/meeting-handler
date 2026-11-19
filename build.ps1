param(
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [string]$QtPath = $(if ($env:QT_MSVC_DIR) { $env:QT_MSVC_DIR } else { 'C:\Qt\6.8.3\msvc2022_64' })
)
$ErrorActionPreference = 'Stop'
$deploy = Join-Path $QtPath 'bin\windeployqt.exe'
if (!(Test-Path -LiteralPath $deploy)) { throw "Qt MSVC kit missing at $QtPath. Specify -QtPath or QT_MSVC_DIR." }

$cmake = (Get-Command cmake.exe -ErrorAction SilentlyContinue).Source
if (!$cmake) {
    $locator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $vs = & $locator -latest -requires Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath
    if (!$vs) { throw 'Install CMake or the Visual Studio CMake component.' }
    $cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$buildDir = Join-Path $PSScriptRoot 'build'
& $cmake -S $PSScriptRoot -B $buildDir -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$QtPath" -DBUILD_TESTING=ON -DMEETING_CONTROLLER_BUILD_TESTS=ON
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
& $cmake --build $buildDir --config $Configuration --parallel 1
if ($LASTEXITCODE) { throw 'Compilation failed.' }

$exe = Join-Path $buildDir "$Configuration\MeetingHandler.exe"
$mode = '--' + $Configuration.ToLowerInvariant()
$deployArgs = @($mode, '--no-system-dxc-compiler')
if ($Configuration -eq 'Release') {
    & $cmake --install $buildDir --config Release --component MsvcRuntime --prefix (Split-Path $exe)
    if ($LASTEXITCODE) { throw 'C++ runtime deployment failed.' }
    $deployArgs += '--no-compiler-runtime'
}
& $deploy @deployArgs $exe
if ($LASTEXITCODE) { throw 'Qt deployment failed.' }
& $ctest --test-dir $buildDir -C $Configuration --output-on-failure
if ($LASTEXITCODE) { throw 'Regression tests failed.' }
Write-Host "Built and tested $exe"
