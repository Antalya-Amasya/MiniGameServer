$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Bin = Join-Path $Root "build\bin"
$ServerExe = Join-Path $Bin "minigame_server.exe"
$ClientExe = Join-Path $Bin "minigame_client.exe"
if (-not (Test-Path $ServerExe) -or -not (Test-Path $ClientExe)) {
    Write-Error "build the project first; missing $ServerExe or $ClientExe"
}

$Work = Join-Path $env:TEMP "minigame-smoke"
New-Item -ItemType Directory -Force -Path $Work | Out-Null
$Port = 19121

@"
LOGIN alice
/wait OK LOGIN alice
CREATE_ROOM
/wait WAITING_OPPONENT
/wait YOUR_TURN
MOVE 0 0
/wait YOUR_TURN
MOVE 0 1
/wait YOUR_TURN
MOVE 0 2
/wait GAME_OVER X_WIN
QUIT
"@ | Set-Content -Path (Join-Path $Work "alice.txt") -Encoding ascii

@"
LOGIN bob
/wait OK LOGIN bob
JOIN_ROOM 1001
/wait OPPONENT_TURN
/wait YOUR_TURN
MOVE 1 0
/wait OPPONENT_TURN
/wait YOUR_TURN
MOVE 1 1
/wait GAME_OVER X_WIN
QUIT
"@ | Set-Content -Path (Join-Path $Work "bob.txt") -Encoding ascii

$ServerOut = Join-Path $Work "server.out"
$ServerErr = Join-Path $Work "server.err"
$AliceOut = Join-Path $Work "alice.out"
$AliceErr = Join-Path $Work "alice.err"
$BobOut = Join-Path $Work "bob.out"
$BobErr = Join-Path $Work "bob.err"

$Server = Start-Process -FilePath $ServerExe -ArgumentList @("$Port") -PassThru -WindowStyle Hidden `
    -RedirectStandardOutput $ServerOut -RedirectStandardError $ServerErr
try {
    $Ready = $false
    for ($i = 0; $i -lt 50; ++$i) {
        if (Test-Path $ServerErr) {
            $text = Get-Content -Path $ServerErr -Raw -ErrorAction SilentlyContinue
            if ($text -match "listening on port $Port") {
                $Ready = $true
                break
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $Ready) {
        throw "server did not start listening"
    }

    $Alice = Start-Process -FilePath $ClientExe -ArgumentList @("127.0.0.1", "$Port") -PassThru -WindowStyle Hidden `
        -RedirectStandardInput (Join-Path $Work "alice.txt") `
        -RedirectStandardOutput $AliceOut -RedirectStandardError $AliceErr
    $null = $Alice.Handle

    $Joined = $false
    for ($i = 0; $i -lt 50; ++$i) {
        if (Test-Path $AliceOut) {
            $text = Get-Content -Path $AliceOut -Raw -ErrorAction SilentlyContinue
            if ($text -match "WAITING_OPPONENT") {
                $Joined = $true
                break
            }
        }
        Start-Sleep -Milliseconds 100
    }
    if (-not $Joined) {
        throw "alice did not create a room"
    }

    $Bob = Start-Process -FilePath $ClientExe -ArgumentList @("127.0.0.1", "$Port") -PassThru -WindowStyle Hidden `
        -RedirectStandardInput (Join-Path $Work "bob.txt") `
        -RedirectStandardOutput $BobOut -RedirectStandardError $BobErr
    $null = $Bob.Handle

    if (-not $Alice.WaitForExit(20000)) { throw "alice client timed out" }
    if (-not $Bob.WaitForExit(20000)) { throw "bob client timed out" }
    # 重定向了标准输出时，带超时的 WaitForExit 返回后还要再等一次，ExitCode 才会就绪。
    $Alice.WaitForExit() | Out-Null
    $Bob.WaitForExit() | Out-Null
    if ($Alice.ExitCode -ne 0) { throw "alice exit code $($Alice.ExitCode)" }
    if ($Bob.ExitCode -ne 0) { throw "bob exit code $($Bob.ExitCode)" }

    $AliceText = Get-Content -Path $AliceOut -Raw
    $BobText = Get-Content -Path $BobOut -Raw
    if ($AliceText -notmatch "GAME_OVER X_WIN") { throw "alice did not see X_WIN" }
    if ($BobText -notmatch "GAME_OVER X_WIN") { throw "bob did not see X_WIN" }
    if ($AliceText -notmatch "XXX") { throw "alice did not see the winning board" }
    if (-not (Get-Process -Id $Server.Id -ErrorAction SilentlyContinue)) {
        throw "server exited before clients finished"
    }
    Write-Output "SMOKE_OK"
}
finally {
    if (Get-Process -Id $Server.Id -ErrorAction SilentlyContinue) {
        Stop-Process -Id $Server.Id -Force
    }
    $Server.WaitForExit() | Out-Null
}
