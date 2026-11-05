<#
.SYNOPSIS
  Regenera ghost_pc/patches a partir da arvore de trabalho (work/ps2recomp_game).

.DESCRIPTION
  1. ps2xRuntime-modified.patch = diff dos arquivos EXISTENTES no PS2Recomp original
     (third_party/PS2Recomp, rastreados pelo git dele) contra a arvore de trabalho, em
     ps2xRuntime, ps2xRecomp, ps2xIOP e CMakeLists.txt. Caminhos a/<rel> b/<rel>, relativos
     a raiz do PS2Recomp (use `git apply --directory=ghost_pc/work/ps2recomp_game`).
  2. Copia para patches/ps2xRuntime (e ps2xRecomp/ps2xIOP, se houver) os arquivos NOVOS
     (inexistentes no original), so de extensoes de codigo-fonte, excluindo src/runner e
     include/ps2_recompiled_* (gerados), binarios, capturas (.rec/.png/.bin...).
  3. Confere `git apply --check` do patch gerado sobre o original e grava logs/sync_patches_*.log.

  Rode de qualquer lugar. Nao comita nada. Arquivos do jogo nao entram (so codigo do runtime).

.EXAMPLE
  powershell -NoProfile -File ghost_pc\tools\sync_patches.ps1 [-DryRun]
#>
param(
    [string]$Orig,                       # padrao: <raiz>\third_party\PS2Recomp
    [string]$Work,                       # padrao: <jogo>\work\ps2recomp_game
    [string]$PatchDir,                   # padrao: <jogo>\patches
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$game = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent $game
if (-not $Orig) { $Orig = Join-Path $root 'third_party\PS2Recomp' }
if (-not $Work) { $Work = Join-Path $game 'work\ps2recomp_game' }
if (-not $PatchDir) { $PatchDir = Join-Path $game 'patches' }
New-Item -ItemType Directory -Force (Join-Path $game 'logs') | Out-Null
$log = Join-Path $game ('logs\sync_patches_{0}.log' -f (Get-Date -Format 'yyyyMMdd_HHmmss'))
function Log([string]$lvl, [string]$msg) {
    $line = '{0} [{1}] {2}' -f (Get-Date -Format 's'), $lvl, $msg
    Write-Host $line
    Add-Content -Encoding utf8 -Path $log -Value $line
}
if (-not (Test-Path (Join-Path $Orig '.git'))) { Log 'ERROR' "original nao encontrado: $Orig (veja patches/README.md)"; exit 2 }
if (-not (Test-Path $Work)) { Log 'ERROR' "arvore de trabalho nao encontrada: $Work"; exit 2 }

$areas = @('ps2xRuntime', 'ps2xRecomp', 'ps2xIOP')
$srcExt = '\.(h|hpp|inl|c|cc|cpp|cxx|py|cmake|txt|hlsl|ps1)$'
# Excluidos dos arquivos novos (gerados / pesados / binarios / dados do jogo).
$skipNew = @('^ps2xRuntime/src/runner/', '^ps2xRuntime/include/ps2_recompiled_', '/out/', '/__pycache__/', '\.(rec|png|bmp|bin|obj|exe|dll|lib|pdb|iso|wav|pch|ilk)$')

$gitOut = & git -C $Orig ls-files -- $areas CMakeLists.txt
$tracked = @{}
foreach ($f in $gitOut) { $tracked[$f] = $true }
Log 'INFO' ("original: {0} arquivos rastreados nas areas" -f $tracked.Count)

# ---- 1. diff dos arquivos existentes ----
$sb = New-Object System.Text.StringBuilder
$changed = 0
$tmp = Join-Path $env:TEMP ('sync_patches_' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $tmp | Out-Null
try {
    foreach ($rel in ($tracked.Keys | Sort-Object)) {
        if ($rel -match '^ps2xRuntime/src/runner/') { continue }
        $w = Join-Path $Work $rel
        $o = Join-Path $Orig $rel
        if (-not (Test-Path $w)) {
            if ($rel -match '^[^/]+/(vita|android)/') { continue }   # a arvore de trabalho exclui vita/android (README)
            Log 'WARN' "removido na arvore de trabalho (ignorado): $rel"; continue
        }
        $wi = Get-Item $w; $oi = Get-Item $o
        if ($wi.Length -eq $oi.Length -and (Get-FileHash $w -Algorithm SHA1).Hash -eq (Get-FileHash $o -Algorithm SHA1).Hash) { continue }
        # diff com caminhos relativos: monta par a/ b/ numa pasta temporaria
        $ta = Join-Path $tmp 'a'; $tb = Join-Path $tmp 'b'
        foreach ($d in @($ta, $tb)) { $p = Join-Path $d $rel; New-Item -ItemType Directory -Force (Split-Path $p) | Out-Null }
        Copy-Item $o (Join-Path $ta $rel); Copy-Item $w (Join-Path $tb $rel)
        Push-Location $tmp
        try {
            $d = & git -c core.autocrlf=false -c core.safecrlf=false diff --no-index --no-color --src-prefix=a/ --dst-prefix=b/ -- ("a/" + $rel) ("b/" + $rel)
        } finally { Pop-Location }
        # `git diff --no-index` mostra a/a/<rel> b/b/<rel>: normaliza
        $d = $d | ForEach-Object { $_ -replace '^(diff --git )a/a/(.*) b/b/(.*)$', '$1a/$2 b/$3' -replace '^--- a/a/', '--- a/' -replace '^\+\+\+ b/b/', '+++ b/' }
        if ($d) { [void]$sb.Append((($d -join "`n") + "`n")); $changed++ }
        Remove-Item -Recurse -Force $ta, $tb
    }
} finally { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
Log 'INFO' "arquivos existentes modificados: $changed"

$patchFile = Join-Path $PatchDir 'ps2xRuntime-modified.patch'
if (-not $DryRun) {
    [System.IO.File]::WriteAllBytes($patchFile, (New-Object System.Text.UTF8Encoding($false)).GetBytes($sb.ToString()))
    Log 'INFO' "escrito: $patchFile ($((Get-Item $patchFile).Length) bytes)"
}

# ---- 2. arquivos novos ----
$new = @()
foreach ($a in $areas) {
    $dir = Join-Path $Work $a
    if (-not (Test-Path $dir)) { continue }
    Get-ChildItem -Recurse -File $dir | ForEach-Object {
        $rel = $_.FullName.Substring($Work.Length + 1).Replace('\', '/')
        if ($tracked.ContainsKey($rel)) { return }
        if ($rel -notmatch $srcExt) { return }
        foreach ($s in $skipNew) { if ($rel -match $s) { return } }
        $new += $rel
    }
}
if (Test-Path (Join-Path $Work 'CMakeLists.txt')) { }
foreach ($rel in $new) {
    $dst = Join-Path $PatchDir $rel
    if (-not $DryRun) {
        New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null
        Copy-Item (Join-Path $Work $rel) $dst -Force
    }
}
Log 'INFO' ("arquivos novos copiados: {0}" -f $new.Count)
$new | ForEach-Object { Log 'INFO' "  novo: $_" }

# arquivos de patches/ sem correspondente na arvore (obsoletos)
Get-ChildItem -Recurse -File (Join-Path $PatchDir 'ps2xRuntime') -ErrorAction SilentlyContinue | ForEach-Object {
    $rel = 'ps2xRuntime/' + $_.FullName.Substring((Join-Path $PatchDir 'ps2xRuntime').Length + 1).Replace('\', '/')
    if (-not (Test-Path (Join-Path $Work $rel))) { Log 'WARN' "patches/ tem arquivo sem origem na arvore (obsoleto?): $rel" }
}

# ---- 3. verificacao ----
if (-not $DryRun) {
    & git -C $Orig apply --check $patchFile 2>&1 | ForEach-Object { Log 'ERROR' "$_" }
    if ($LASTEXITCODE -ne 0) { Log 'ERROR' 'git apply --check FALHOU sobre o original'; exit 1 }
    Log 'INFO' 'git apply --check OK sobre o original'
    # protecao (regra 2): nada de capturas/binarios em patches/
    $bad = Get-ChildItem -Recurse -File $PatchDir | Where-Object { $_.Extension -match '^\.(rec|png|bmp|bin|exe|dll|obj|iso|wav)$' }
    if ($bad) { $bad | ForEach-Object { Log 'ERROR' "arquivo proibido em patches/: $($_.FullName)" }; exit 1 }
}
Log 'INFO' 'ok'
exit 0
