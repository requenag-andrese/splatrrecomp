# One game run for tests: a copy of a base profile, muted, an input script, a picture series; ends by closing the window.
# Outputs in <runs>/<tag>: f<frame>.png, and <tag>.err / .out (the log) and <tag>.ini (the settings it ran with).
#   run.ps1 -Tag hub1 -Script ink -Seconds 60 -Shots 1100-1300:1
# -Script: a file of input/ by name (still, pan, ink, ink2) or a path. -Profile: a base profile of <profiles> (hub, start),
# copied to <runs>/prof-<tag> first unless -KeepProfile (then the run plays the base itself and its save changes).
# -Fullscreen: 2560x1440 full screen instead of a 1920x1080 window. -Full: full-size pictures. Settings of the run's own
# sr-config.ini: dlaa on, sound off; anything else through SR_* environment variables, which override it.
param([Parameter(Mandatory)][string]$Tag, [string]$Script, [int]$Seconds = 60, [string]$Shots = '0-40000:60',
      [string]$Profile = 'hub', [switch]$KeepProfile, [switch]$Fullscreen, [switch]$Full)
. "$PSScriptRoot\common.ps1"
if (Test-GameOpen) { 'sr.exe is open: no test'; exit 2 }
$out = Get-RunDir $Tag
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $out | Out-Null
$base = Join-Path $Profiles $Profile
if (-not (Test-Path $base)) { "no base profile $base"; exit 2 }
$prof = $base
if (-not $KeepProfile) {
    $prof = Join-Path $Runs "prof-$Tag"
    Remove-Item -Recurse -Force $prof -ErrorAction SilentlyContinue
    Copy-Item -Recurse $base $prof
}
$ini = Join-Path $Runs "$Tag.ini"
$display = if ($Fullscreen) { "fullscreen = 1`r`nwindow_size = 2560x1440" } else { "fullscreen = 0`r`nwindow_size = 1920x1080" }
Set-Content $ini "[display]`r`n$display`r`ndlaa = 1`r`n[audio]`r`nvolume = 0`r`n" -Encoding ascii
$env:SR_CONFIG_FILE = $ini; $env:SR_PROFILE_DIR = $prof; $env:SR_BACKGROUND_WINDOW = '1'
$env:SR_SHOTS = $Shots; $env:SR_SHOTS_DIR = $out
if ($Full) { $env:SR_SHOTS_FULL = '1' } else { Remove-Item Env:SR_SHOTS_FULL -ErrorAction SilentlyContinue }
Remove-Item Env:SR_INPUT_SCRIPT -ErrorAction SilentlyContinue
if ($Script) {
    $path = if (Test-Path $Script) { (Resolve-Path $Script).Path } else { Join-Path $TestDir "input\$Script.txt" }
    if (-not (Test-Path $path)) { "no input script $Script"; exit 2 }
    $env:SR_INPUT_SCRIPT = $path
}
$p = Start-Process -FilePath (Join-Path $Build 'sr.exe') -ArgumentList '--play' -WorkingDirectory $Build -PassThru `
    -RedirectStandardOutput (Join-Path $Runs "$Tag.out") -RedirectStandardError (Get-RunLog $Tag)
for ($i = 0; $i -lt 300; $i++) { $p.Refresh(); if ($p.MainWindowHandle -ne 0 -or $p.HasExited) { break }; Start-Sleep -Milliseconds 100 }
for ($i = 0; $i -lt $Seconds -and -not $p.HasExited; $i++) { Start-Sleep -Seconds 1; $p.Refresh() }
"alive at the end: $(-not $p.HasExited)"
Close-Game 90
foreach ($v in 'SR_CONFIG_FILE', 'SR_PROFILE_DIR', 'SR_BACKGROUND_WINDOW', 'SR_SHOTS', 'SR_SHOTS_DIR', 'SR_SHOTS_FULL', 'SR_INPUT_SCRIPT') {
    Remove-Item "Env:$v" -ErrorAction SilentlyContinue
}
Select-String -Path (Get-RunLog $Tag) -Pattern 'FATAL|guest frame +[0-9]:|frametime: exit' | Select-Object -First 12 |
    ForEach-Object { $_.Line.Substring(0, [Math]::Min(160, $_.Line.Length)) }
