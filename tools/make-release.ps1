<#
.SYNOPSIS
  Packs both games into release_<Version>.zip: a "Zero Hour" folder and a "Generals" folder, each with the 64-bit program, the DXVK DLLs,
  the launchers and helper scripts, plus a README-INSTALL.txt in the root.

.DESCRIPTION
  The zip contains no files of the original games. Unzip it anywhere, then copy the content of the folder that matches the game
  into the game folder (see README-INSTALL.txt inside).
  Needs finished builds of both games (scriptsuild-x64.ps1 -Video, once with -Target z_generals and once with -Target g_generals)
  and the DXVK build (tools\dxvkuild-dxvk.ps1).

.EXAMPLE
  powershell -File tools\make-release.ps1 -Version 0.3.0
#>
param([Parameter(Mandatory = $true)][string]$Version, [string]$BuildRoot = "", [string]$DxvkDir = "", [string]$Out = "")
$ErrorActionPreference = "Stop"
$repo = Split-Path $PSScriptRoot -Parent
if (-not $BuildRoot) { $BuildRoot = Join-Path $repo "build\win64-vcpkg" }
if (-not $DxvkDir) { $DxvkDir = Join-Path $PSScriptRoot "dxvk\work\out" }
if (-not $Out) { $Out = Join-Path $repo "ZeroHourReimagined_$Version.zip" }
# GPL v3: the source of the release goes out with it, as a second zip made from the committed tree
$SourceName = "ZeroHourReimagined_$($Version)_source.zip"
$sourceOut = Join-Path (Split-Path $Out -Parent) $SourceName
$zhBuild = Join-Path $BuildRoot "GeneralsMD\Release"
$gBuild = Join-Path $BuildRoot "Generals\Release"
if (-not (Test-Path (Join-Path $zhBuild "generalszh.exe"))) { throw "No Zero Hour build in $zhBuild. Run scriptsuild-x64.ps1 -Video first." }
if (-not (Test-Path (Join-Path $gBuild "generalsv.exe"))) { throw "No Generals build in $gBuild. Run scriptsuild-x64.ps1 -Video -Target g_generals first." }
if (-not (Test-Path (Join-Path $DxvkDir "d3d8.dll"))) { throw "No DXVK build in $DxvkDir. Run tools\dxvkuild-dxvk.ps1 first." }

$stage = Join-Path $repo "build\release-staging"
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item (Join-Path $repo "LICENSE.md"), (Join-Path $repo "THIRD_PARTY_NOTICES.md") $stage

function New-GameFolder($name, $gameTitle, $buildDir, $exeIn, $exeOut)
{
    $dir = Join-Path $stage $name
    $x64 = Join-Path $dir "x64"
    New-Item -ItemType Directory -Force $x64, (Join-Path $dir "tools") | Out-Null
    Copy-Item (Join-Path $buildDir $exeIn) (Join-Path $x64 $exeOut)
    # the video libraries are built next to the Zero Hour program
    Get-ChildItem $zhBuild -Filter *.dll | ForEach-Object { Copy-Item $_.FullName $x64 }
    foreach ($d in "d3d8.dll", "d3d9.dll", "dxgi.dll") { Copy-Item (Join-Path $DxvkDir $d) $x64 }
    foreach ($f in "bigtool.py", "patch_options_wnd.py") { Copy-Item (Join-Path $PSScriptRoot $f) (Join-Path $dir "tools") }
    $common = "@echo off`r`nrem 64-bit $gameTitle. -useCwd keeps this folder as the working directory (loose Data\Cursors and Data\Movies are read from here).`r`ncd /d `"%~dp0`"`r`n"
    Set-Content (Join-Path $dir "Play-Updated.bat") ($common + "rem Updated graphics: native Vulkan renderer with all enhancements (Options.ini can switch single effects off). Add -win for a window.`r`n`"%~dp0x64\$exeOut`" -useCwd %*`r`n") -Encoding ASCII
    Set-Content (Join-Path $dir "Play-Original.bat") ($common + "rem Original rendering: Direct3D 8 through DXVK, no added effects. Add -win for a window.`r`nset GENERALS_GFX=d3d8`r`nset GENERALS_ORIGINAL=1`r`n`"%~dp0x64\$exeOut`" -useCwd %*`r`n") -Encoding ASCII
    Set-Content (Join-Path $dir "Install-OptionsMenu.bat") "@echo off`r`nrem Optional: adds the graphics check boxes to the in-game options menu. Needs Python 3. Reads the menu from your own game files and writes a patched copy next to them.`r`ncd /d `"%~dp0`"`r`npython tools\patch_options_wnd.py `"%~dp0.`"`r`nif errorlevel 1 echo Failed: is Python 3 installed and is this the $gameTitle folder?`r`npause`r`n" -Encoding ASCII
    # scorch mark textures (generated, no game data): colour atlas and height map for the crater lighting
    & python (Join-Path $PSScriptRoot "gen_scorch.py") $dir | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "gen_scorch.py failed (needs Python 3 with numpy and Pillow)" }
    # the intro logo of this mod (made by make_intro_logo.py, shown after the EA logo)
    $textures = Join-Path $dir "Art\Textures"
    New-Item -ItemType Directory -Force $textures | Out-Null
    Copy-Item (Join-Path $repo "assets\Art\Textures\ModIntroLogo.tga") $textures
    # optional helpers: skip the startup movies for a quick start (moved, not deleted) and put them back
    Copy-Item (Join-Path $PSScriptRoot "release\Skip-IntroMovies.bat"), (Join-Path $PSScriptRoot "release\Restore-IntroMovies.bat") $dir
}
New-GameFolder "Zero Hour" "Zero Hour" $zhBuild "generalszh.exe" "generalszh64.exe"
New-GameFolder "Generals" "Generals" $gBuild "generalsv.exe" "generalsv64.exe"

Set-Content (Join-Path $stage "README-INSTALL.txt") @"
Command & Conquer: Generals - Zero Hour Reimagined, version $Version
by dement123 (Nexus Mods)

A 64-bit program with a Vulkan renderer for Zero Hour and the base game Generals, with gameplay changes for Zero Hour.

What is new
- Graphics: new water, real shadows, ambient occlusion, bloom, anti-aliasing, dynamic lights around explosions, clouds.
  Play-Original.bat keeps the original look.
- Zero Hour gameplay: faster economy (supply docks hold 10x more), lower costs, faster building, unit formations and a
  stronger Hard skirmish AI that expands and attacks in waves.
- Smooth large battles: a faster pathfinder (4v4 games stay fluid) and fixes for units piling up at cliffs.
- Fixes: game speed slider in skirmish, unit voice sounds, text cursor, radiation and toxin pools on the ground.
- Starts fullscreen at the resolution of your monitor.

REQUIREMENTS
- Your own installed copy of Command & Conquer: Generals - Zero Hour 1.04 (EA app, Steam or the original discs, patched).
  The base game Generals 1.08 is optional. This archive contains no game files.
- Windows 10 or 11, 64-bit.
- A graphics card with a Vulkan 1.3 driver (NVIDIA GTX 900 series or newer, AMD RX 400 or newer, Intel Arc or recent iGPU).
  Update the graphics driver first if you are not sure.

WHAT IS IN THIS ARCHIVE
    Zero Hour\   for Command & Conquer: Generals - Zero Hour (everything of this mod)
    Generals\    for Command & Conquer: Generals, the base game (graphics and engine fixes only, no gameplay changes)

INSTALL (Zero Hour)
1. Find your Zero Hour game folder: the folder that contains INIZH.big. Examples:
       EA app:  C:\Program Files\EA Games\Command and Conquer Generals Zero Hour\Command and Conquer Generals Zero Hour
       Steam:   the Zero Hour folder inside ...\steamapps\common
   If you are not sure, search your disk for INIZH.big (or use Browse local files in Steam / the EA app).
2. Make a backup copy of that folder if you want to be on the safe side (the mod overwrites no game file, it only adds files).
3. Open the "Zero Hour" folder of this archive and copy its CONTENT (the folders x64, Art and tools and all .bat files, not the
   "Zero Hour" folder itself) into the game folder from step 1. Confirm merging the folders if Windows asks.
4. Start the game with Play-Updated.bat in the game folder. Create a desktop shortcut to it if you like
   (right click > Send to > Desktop).
   Do not start the .exe in the x64 folder directly and do not copy it over an original game file.
5. Optional add-ons from the same Nexus page (install them after the main mod):
   - HD Textures: copy the Art folder from its "Zero Hour" folder into the game folder and confirm merging.
   - No Fog / Far Zoom launchers: copy the .bat files from its "Zero Hour" folder into the game folder and start the game
     with Play-NoFog.bat, Play-FarZoom.bat or Play-NoFog-FarZoom.bat instead of Play-Updated.bat.
   All add-ons work together.

INSTALL (Generals, the base game; optional)
Same as above with the "Generals" folder of this archive and the Generals game folder (the one with INI.big; with the EA app
it is usually next to the Zero Hour folder: ...\Command and Conquer Generals Zero Hour\Command and Conquer Generals).
Do not mix them: Zero Hour files go to the Zero Hour folder, Generals files to the Generals folder.

LAUNCHERS
    Play-Updated.bat    the mod with the new Vulkan graphics (use this one)
    Play-Original.bat   the mod with the original look of the game (Direct3D 8 through DXVK), for comparison or old PCs

SETTINGS
- The game starts fullscreen at the resolution of your monitor. Change it in Options > Display, or in Options.ini.
  For a window, edit the .bat file and add -win at the end of the line that starts the game.
- Options.ini is in Documents\Command and Conquer Generals Zero Hour Data (one per game). Useful keys:
      Resolution = 2560 1440        screen resolution
      FogOfWar = no                 whole map visible (same as Play-NoFog.bat)
      MaxCameraHeight = 750         how far the camera zooms out (mod default 450, original game 310)
      GraphicsMode = classic        original look in Play-Updated.bat too
- Optional: Install-OptionsMenu.bat (needs Python 3) adds check boxes for the new graphics effects to
  Options > Advanced display options. Without it every effect can still be switched in Options.ini.
- Optional: Skip-IntroMovies.bat starts the game faster without the EA logo and trailer (the movies are moved to
  IntroMovies_off, nothing is deleted); Restore-IntroMovies.bat puts them back.

MULTIPLAYER AND REPLAYS
The gameplay changes make games differ from the original game. Everyone in a LAN game must use this same version of the mod,
and replays only play back with the version that recorded them. It does not work with players on the original game.

TROUBLESHOOTING
- Black screen or the game closes at start: update your graphics driver (Vulkan 1.3 is needed), or try Play-Original.bat.
- Fullscreen problems: add -win to the .bat file and play in a window.
- Low frame rate on a weak PC: set GraphicsMode = classic in Options.ini or use Play-Original.bat.
- The game is not found / missing files: the files must be in the folder with INIZH.big, not in a subfolder.

UNINSTALL
Delete what you copied into the game folder: the x64 and tools folders, the .bat files, the files of this mod in
Art\Textures (ModIntroLogo.tga, exscorch01.dds, exscorch01_h.dds and the HD textures if you installed them; or the whole Art
folder if no other mod uses it) and, if you used Install-OptionsMenu.bat, the Window folder. If you used Skip-IntroMovies.bat,
run Restore-IntroMovies.bat first. Your original game files are untouched.

Credits: dement123 (this version, code written with Claude Code); the 64-bit Vulkan fork it continues
(https://github.com/JaChriCaeTob/ModernGeneralsZH); TheSuperHackers/GeneralsGameCode community project; EA for the original
game and its source code release.
Licenses: GPL v3 with EA's additional terms (LICENSE.md), THIRD_PARTY_NOTICES.md (DXVK: zlib license, FFmpeg: LGPL).
The source code of this version is available as a separate download: $SourceName
"@ -Encoding ASCII

if (Test-Path $Out) { Remove-Item $Out -Force }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $Out -CompressionLevel Optimal
Write-Host ("Wrote {0} ({1:N1} MB)" -f $Out, ((Get-Item $Out).Length / 1MB))

# Optional launchers: no fog of war and a farther zoom, for both games. They start the same program, so they work together
# with the HD textures; Options.ini (FogOfWar = no, MaxCameraHeight = ...) does the same for every launcher.
$optName = "ZeroHourReimagined_$($Version)_NoFog_FarZoom.zip"
$optOut = Join-Path (Split-Path $Out -Parent) $optName
$optStage = Join-Path $repo "build\release-optional"
if (Test-Path $optStage) { Remove-Item $optStage -Recurse -Force }
foreach ($g in @(@("Zero Hour", "generalszh64.exe"), @("Generals", "generalsv64.exe")))
{
    $dir = Join-Path $optStage $g[0]
    New-Item -ItemType Directory -Force $dir | Out-Null
    $head = "@echo off`r`nrem Zero Hour Reimagined, optional launcher. Needs the main mod in this folder.`r`ncd /d `"%~dp0`"`r`n"
    $exe = "`"%~dp0x64\$($g[1])`" -useCwd"
    Set-Content (Join-Path $dir "Play-NoFog.bat") ($head + "rem The whole map is visible: no shroud and no fog of war.`r`n$exe -noFog %*`r`n") -Encoding ASCII
    Set-Content (Join-Path $dir "Play-FarZoom.bat") ($head + "rem The camera zooms out farther (750 instead of 450; the original game had 310).`r`n$exe -maxZoom 750 %*`r`n") -Encoding ASCII
    Set-Content (Join-Path $dir "Play-NoFog-FarZoom.bat") ($head + "rem No fog of war and the farther zoom together.`r`n$exe -noFog -maxZoom 750 %*`r`n") -Encoding ASCII
}
Set-Content (Join-Path $optStage "README-Optional.txt") @"
Command & Conquer: Generals - Zero Hour Reimagined: optional launchers, version $Version
by dement123 (Nexus Mods)

Install the main mod first. Then copy the .bat files from "Zero Hour" into your Zero Hour game folder (and from "Generals"
into your Generals folder, if you use it) and start the game with one of them instead of Play-Updated.bat:

    Play-NoFog.bat           the whole map is visible: no black shroud and no fog of war
    Play-FarZoom.bat         the camera zooms out farther (750 instead of 450; the original game had 310)
    Play-NoFog-FarZoom.bat   both

They start the same program as Play-Updated.bat, so the HD textures and everything else of the mod stay in use.
Instead of a launcher you can also set this in Options.ini (Documents\Command and Conquer ... Data\Options.ini):

    FogOfWar = no
    MaxCameraHeight = 750

Very far zoom costs performance on large maps. No fog only changes what you see, not the game itself, so games and replays
stay the same. In multiplayer it works too: every player decides for himself, and the others cannot see that.
"@ -Encoding ASCII
if (Test-Path $optOut) { Remove-Item $optOut -Force }
Compress-Archive -Path (Join-Path $optStage "*") -DestinationPath $optOut -CompressionLevel Optimal
Write-Host ("Wrote {0} ({1:N1} KB)" -f $optOut, ((Get-Item $optOut).Length / 1KB))

if (Test-Path $sourceOut) { Remove-Item $sourceOut -Force }
git -C $repo archive --format=zip --prefix="ZeroHourReimagined_$($Version)_source/" -o $sourceOut HEAD -- . ":(exclude)release*.zip"
if ($LASTEXITCODE -ne 0) { throw "git archive failed" }
Write-Host ("Wrote {0} ({1:N1} MB)" -f $sourceOut, ((Get-Item $sourceOut).Length / 1MB))
