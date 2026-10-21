[CmdletBinding()]
param(
    [ValidatePattern('^COM[0-9]+$')][string]$Port,
    [string]$IdfPath,
    [ValidateRange(9600, 2000000)][int]$Baud = 460800
)
$ErrorActionPreference = 'Stop'

if (!$IdfPath) {
    $candidates = @($env:IDF_PATH, (Join-Path $env:USERPROFILE 'esp\esp-idf'))
    $IdfPath = $candidates | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'export.ps1')) } | Select-Object -First 1
}
if (!$IdfPath -or !(Test-Path -LiteralPath (Join-Path $IdfPath 'export.ps1'))) {
    throw 'ESP-IDF が見つかりません。-IdfPath でインストール先を指定してください。'
}
Write-Host '[1/3] ESP-IDF を準備しています'
. (Join-Path $IdfPath 'export.ps1')
if (!$Port) {
    $portJson = & python -c 'import json; from serial.tools import list_ports; print(json.dumps([p.device for p in list_ports.comports() if p.vid == 0x303A and p.pid == 0x1001]))'
    if ($LASTEXITCODE -ne 0) { throw 'COMポートの検出に失敗しました。' }
    $ports = @($portJson | ConvertFrom-Json)
    if ($ports.Count -ne 1) {
        throw "ESP32-S3 の接続先を一意に検出できませんでした（候補: $($ports -join ', ')）。USB接続を確認し、必要なら -Port COM14 を指定してください。"
    }
    $Port = $ports[0]
}
Write-Host "書き込み先: $Port"
Push-Location (Join-Path $PSScriptRoot 'board-test\firmware')
try {
    Write-Host '[2/3] ファームウェアをビルドしています'
    & python (Join-Path $IdfPath 'tools\idf.py') build
    if ($LASTEXITCODE -ne 0) { throw 'ビルドに失敗したため、書き込みは行いませんでした。' }
    Write-Host '[3/3] 書き込み・照合・再起動しています'
    & python (Join-Path $IdfPath 'tools\idf.py') -p $Port -b $Baud flash
    if ($LASTEXITCODE -ne 0) {
        throw '書き込みに失敗しました。GUIやシリアルモニターを切断してください。接続できない場合はBOOTを押したままENを押して離し、BOOTを離して再実行してください。'
    }
    Write-Host '書き込みと照合が完了しました。通常起動しない場合はBOOTを離してENを押してください。' -ForegroundColor Green
} finally {
    Pop-Location
}
