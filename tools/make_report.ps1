<#
.SYNOPSIS
  Regenera os graficos SVG do relatorio de desempenho (docs\img\*.svg) a partir de docs\perf\data.json.

.DESCRIPTION
  1. (opcional, -Summaries) nao altera o JSON: edite docs\perf\data.json com os numeros finais e rode de novo.
  2. Roda ..\tools\report_charts.py (stdlib pura) com --base = pasta do jogo (caminhos csv relativos a ela).
  3. Valida que cada SVG e XML bem formado e tem texto (title/desc) legivel; log em logs\make_report.log.
  Rode a partir de qualquer pasta; o script se localiza pela propria pasta.

.EXAMPLE
  powershell -NoProfile -File ghost_pc\tools\make_report.ps1
#>
[CmdletBinding()]
param([string]$Data = 'docs\perf\data.json', [string]$Out = 'docs\img')
$ErrorActionPreference = 'Stop'
$game = Split-Path -Parent $PSScriptRoot
$charts = Join-Path (Split-Path -Parent $game) 'tools\report_charts.py'
Set-Location $game
[void](New-Item -ItemType Directory -Force logs)
$log = Join-Path $game 'logs\make_report.log'
function Write-Log([string]$Level, [string]$Message) {
    $line = '{0} {1} make_report: {2}' -f (Get-Date -Format 'yyyy-MM-ddTHH:mm:ss'), $Level, $Message
    Add-Content -Encoding UTF8 -Path $log -Value $line; Write-Host $line
}

python -I $charts --data $Data --out $Out --base .
if ($LASTEXITCODE -ne 0) { Write-Log 'ERROR' "report_charts.py falhou ($LASTEXITCODE)"; exit 1 }

$bad = 0
foreach ($svg in Get-ChildItem (Join-Path $game $Out) -Filter *.svg) {
    try {
        $xml = New-Object Xml.XmlDocument; $xml.Load($svg.FullName)
        $texts = $xml.GetElementsByTagName('text').Count
        if ($texts -lt 3 -or -not $xml.DocumentElement.GetElementsByTagName('title').Count) { throw "pouco texto ($texts) ou sem <title>" }
        Write-Log 'INFO' ("ok {0} ({1} textos, {2} KB)" -f $svg.Name, $texts, [Math]::Round($svg.Length / 1KB, 1))
    } catch { $bad++; Write-Log 'ERROR' "$($svg.Name): $($_.Exception.Message)" }
}
if ($bad) { exit 1 }
Write-Log 'INFO' 'graficos atualizados'
