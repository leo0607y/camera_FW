param([switch]$Demo)
$ErrorActionPreference = 'Stop'
$guiPython = Join-Path $PSScriptRoot '.venv\Scripts\pythonw.exe'
if (!(Test-Path -LiteralPath $guiPython)) {
    python -m venv "$PSScriptRoot\.venv"
    if ($LASTEXITCODE -ne 0) { throw 'Python仮想環境を作成できません' }
}
& "$PSScriptRoot\.venv\Scripts\python.exe" -c 'import importlib.util, sys; sys.exit(0 if all(importlib.util.find_spec(m) for m in ("serial", "PIL")) else 1)'
if ($LASTEXITCODE -ne 0) {
    & "$PSScriptRoot\.venv\Scripts\python.exe" -m pip install -r "$PSScriptRoot\gui\requirements.txt"
    if ($LASTEXITCODE -ne 0) { throw 'GUIの依存パッケージを導入できません' }
}
$guiArguments = @('"' + "$PSScriptRoot\gui\board_test.py" + '"')
if ($Demo) { $guiArguments += '--demo' }
Start-Process -FilePath $guiPython -ArgumentList $guiArguments -WindowStyle Hidden
