param([string]$Port, [string]$IdfPath, [int]$Baud = 460800)
& "$PSScriptRoot\..\write-firmware.ps1" @PSBoundParameters
