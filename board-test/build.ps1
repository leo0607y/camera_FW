param([string]$IdfPath = 'C:\Users\reoch\esp\esp-idf')
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath "$IdfPath\export.ps1")) { throw "ESP-IDF が見つかりません: $IdfPath" }
. "$IdfPath\export.ps1"
Push-Location "$PSScriptRoot\firmware"
try {
    idf.py build
    if ($LASTEXITCODE -ne 0) { throw 'ビルドに失敗しました' }
} finally { Pop-Location }
