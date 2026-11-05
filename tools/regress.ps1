<#
.SYNOPSIS
  Regressao visual do port: roda um pad script com um executavel e compara as capturas com a referencia.

.EXAMPLE
  powershell -File tools\regress.ps1 -Scenario menu                      # build atual vs referencia
  powershell -File tools\regress.ps1 -Scenario scene3d -Exe work\baseline_exe\ps2EntryRunner.exe
  powershell -File tools\regress.ps1 -Scenario menu -Record              # (re)gera a referencia (use o baseline!)

  Rode a partir de ghost_pc/ ou de qualquer lugar (o script entra em ghost_pc).
  Codigo de saida: 0 = identico dentro da tolerancia; 1 = diferenca; 2 = falha de execucao.
#>
param(
    [ValidateSet('menu', 'scene3d')][string]$Scenario = 'menu',
    [string]$Exe = 'work\ps2recomp_game\out\build_perf\ps2xRuntime\ps2EntryRunner.exe',
    [string]$PadScript,                 # padrao: pad_scripts\bench_menu.txt / regress_scene3d.txt
    [string]$Ref,                       # padrao: work\reference_regress\<cenario>
    [string]$Out,                       # padrao: work\regress_out\<cenario>_<carimbo>
    [int]$TimeoutSec = 0,               # 0 = 120 (menu) ou 900 (scene3d)
    [ValidateSet('cpu', 'd3d11')][string]$Renderer = 'cpu',
    [ValidateSet('guest', 'host')][string]$PadClock = 'guest',
    [int]$MaxErr = 0,
    [int]$MaxDiffPx = 0,
    [double]$MinPsnr = 0,
    [switch]$Record,                    # grava direto na referencia (nao compara)
    [switch]$KeepOut
)
$ErrorActionPreference = 'Stop'
$game = Split-Path -Parent $PSScriptRoot
Set-Location $game
$root = Split-Path -Parent $game
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
New-Item -ItemType Directory -Force logs | Out-Null
$log = "logs\regress_$stamp.log"
function Log([string]$lvl, [string]$msg) {
    $line = "{0} [{1}] {2}" -f (Get-Date -Format 's'), $lvl, $msg
    Write-Host $line
    Add-Content -Encoding utf8 -Path $log -Value $line
}

if (-not $PadScript) { $PadScript = if ($Scenario -eq 'menu') { 'pad_scripts\bench_menu.txt' } else { 'pad_scripts\regress_scene3d.txt' } }
if (-not $Ref) { $Ref = "work\reference_regress\$Scenario" }
if ($Record) { $Out = $Ref } elseif (-not $Out) { $Out = "work\regress_out\${Scenario}_$stamp" }
if ($TimeoutSec -le 0) { $TimeoutSec = if ($Scenario -eq 'menu') { 120 } else { 900 } }
foreach ($p in @($Exe, $PadScript, 'work\SLES_530.20')) {
    if (-not (Test-Path $p)) { Log 'ERROR' "nao encontrado: $p"; exit 2 }
}
if ($Record -and (Test-Path $Out)) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

# Ambiente deterministico: relogio do pad por vsync de GUEST (nao por quadro do host), sem limite
# de FPS (rapido; o jogo mede tempo pelo vsync emulado, nao pelo relogio de parede), sem heartbeat.
$envMap = @{
    PS2X_VIDEO_HZ   = '50'
    PS2X_TARGET_FPS = '0'
    PS2X_PAD_CLOCK  = $PadClock
    PS2X_PAD_SCRIPT = (Resolve-Path $PadScript).Path
    PS2X_SHOT_DIR   = (Resolve-Path $Out).Path
    PS2X_GS         = $Renderer
}
$exeFull = (Resolve-Path $Exe).Path
$run = "logs\regress_run_$stamp.log"
Log 'INFO' "exe=$exeFull cenario=$Scenario script=$PadScript saida=$Out timeout=${TimeoutSec}s env=$(($envMap.GetEnumerator() | Sort-Object Name | ForEach-Object { $_.Name + '=' + $_.Value }) -join ' ')"

foreach ($k in $envMap.Keys) { Set-Item -Path "Env:$k" -Value $envMap[$k] }
$proc = Start-Process -FilePath $exeFull -ArgumentList @('work\SLES_530.20') -WorkingDirectory $game `
    -RedirectStandardOutput $run -RedirectStandardError "$run.err" -PassThru -WindowStyle Minimized
$sw = [Diagnostics.Stopwatch]::StartNew()
if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    Log 'ERROR' "timeout de ${TimeoutSec}s: encerrando o processo"
    Stop-Process -Id $proc.Id -Force
    $proc.WaitForExit()
    $timedOut = $true
}
if (-not $timedOut) { $proc.WaitForExit() }
Log 'INFO' ("processo terminou em {0:N0}s (codigo {1})" -f $sw.Elapsed.TotalSeconds, $proc.ExitCode)
$expected = @(Select-String -Path $PadScript -Pattern '^\s*\d+\s+shot\s+(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value })
$got = @(Get-ChildItem $Out -Filter *.png -ErrorAction SilentlyContinue | ForEach-Object { $_.BaseName })
$missing = @($expected | Where-Object { $got -notcontains $_ })
if ($timedOut -or $missing.Count -gt 0) {
    Log 'ERROR' "capturas faltando ($($missing.Count)/$($expected.Count)): $($missing -join ', ')"
    exit 2
}
Log 'INFO' "$($got.Count) capturas em $Out"
if ($Record) { Log 'INFO' "referencia gravada em $Ref"; exit 0 }

python -I "$root\tools\image_regress.py" $Ref $Out --logs logs --name "regress_$Scenario" --max-err $MaxErr --max-diff-px $MaxDiffPx --min-psnr $MinPsnr
$code = $LASTEXITCODE
Log 'INFO' "image_regress codigo $code ($(if ($code -eq 0) { 'OK' } else { 'DIFERENCAS' }))"
if (-not $KeepOut -and $code -eq 0) { Remove-Item -Recurse -Force $Out }
exit $code
