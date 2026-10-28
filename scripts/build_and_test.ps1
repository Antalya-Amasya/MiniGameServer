$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
Set-Location $Root

# MinGW 的 ld 不能处理带空格的安装路径。优先使用没有空格的目录。
foreach ($dir in @("C:\mingw64\bin")) {
    if (Test-Path (Join-Path $dir "g++.exe")) {
        $env:Path = "$dir;" + $env:Path
        break
    }
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    $wingetCmake = Get-ChildItem -Path "$env:LOCALAPPDATA\Microsoft\WinGet\Packages" -Filter cmake.exe -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if ($wingetCmake) {
        $env:Path = "$(Split-Path $wingetCmake.FullName -Parent);" + $env:Path
    }
}

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Error "cmake was not found on PATH"
}
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    Write-Error "g++ was not found on PATH. Install MinGW-w64 into a directory without spaces."
}

cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Debug
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

ctest --test-dir build --output-on-failure
exit $LASTEXITCODE
