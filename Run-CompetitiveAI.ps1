<#
.SYNOPSIS
Launch the built Zero Hour engine with an existing game installation as data.
.DESCRIPTION
Select Competitive AI after Brutal AI in the Skirmish player dropdown.
This launcher does not replace any file in the installed game directory.
#>
[CmdletBinding()]
param(
    [string] $GameDirectory = 'C:\Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour',
    [string] $Executable = (Join-Path $PSScriptRoot 'build\win32\GeneralsMD\Release\generalszh.exe')
)

$ErrorActionPreference = 'Stop'
$aiGameDirectory = (Resolve-Path -LiteralPath $GameDirectory).Path
$aiExecutable = (Resolve-Path -LiteralPath $Executable).Path
if (-not (Test-Path -LiteralPath (Join-Path $aiGameDirectory 'INIZH.big') -PathType Leaf)) {
    throw 'The selected directory does not contain Zero Hour game data (INIZH.big).'
}
if (-not (Test-Path -LiteralPath $aiExecutable -PathType Leaf)) {
    throw 'Build the Zero Hour executable before running this launcher.'
}
Write-Host 'Starting the built engine. In Skirmish, select Competitive AI after Brutal AI.'
Start-Process -FilePath $aiExecutable -WorkingDirectory $aiGameDirectory `
    -ArgumentList @('-win', '-quickstart', '-setCwd', ('"' + $aiGameDirectory + '"'))
