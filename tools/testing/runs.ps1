# Comparable runs: the same input script under virtual time (SR_VIRTUAL_TIME=0: the guest clock follows the presents,
# random entropy and date fixed), each on a fresh copy of the base profile, so two runs step the game alike and their
# pictures can be compared frame by frame (offset.ps1 finds the load-length offset between them first).
#   runs.ps1 -Tags a1,a2 -Script ink -Shots 1100-1300:1
# A tag starting with "ed" runs with eden-xlate (<build>/eden/eden-xlate.exe, translator study) on its own shader cache
# (<runs>/cache-<tag>) and pipeline files (<runs>/eden-pc, seeded once with the build's recorded programs), with
# SR_TXD_TRANSPOSE=0; -Stages vs|ps and -Keys <hex digits> limit what it translates (the rest goes to sr-xlate).
# -RealTime: without virtual time. Extra SR_* variables set before the call reach every run.
param([Parameter(Mandatory)][string[]]$Tags, [string]$Script = 'ink', [string]$Shots = '1100-1300:1', [int]$Seconds = 50,
      [string]$Profile = 'hub', [string]$Stages = '', [string]$Keys = '', [switch]$RealTime)
. "$PSScriptRoot\common.ps1"
if (Test-GameOpen) { 'sr.exe is open: no test'; exit 2 }
$vars = 'EDEN_XLATE_STAGES', 'EDEN_XLATE_KEYS', 'SR_XLATE', 'SR_SHADER_CACHE', 'SR_TXD_TRANSPOSE', 'SR_PIPELINE_CACHE', 'SR_VIRTUAL_TIME'
foreach ($t in $Tags) {
    foreach ($v in $vars) { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
    if (-not $RealTime) { $env:SR_VIRTUAL_TIME = '0' }
    if ($t -like 'ed*') {
        $pc = Join-Path $Runs 'eden-pc'
        if (-not (Test-Path $pc)) {   # the recorded programs, so the boot prewarm knows every shader the build met
            New-Item -ItemType Directory -Force $pc | Out-Null
            foreach ($f in 'shader-answers.bin', 'shader-programs.bin') {
                if (Test-Path (Join-Path $Build $f)) { Copy-Item (Join-Path $Build $f) $pc }
            }
        }
        $env:SR_XLATE = Join-Path $Build 'eden\eden-xlate.exe'
        $env:SR_SHADER_CACHE = Join-Path $Runs "cache-$t"
        $env:SR_TXD_TRANSPOSE = '0'
        $env:SR_PIPELINE_CACHE = Join-Path $pc 'pipeline-cache.bin'
        if ($Stages) { $env:EDEN_XLATE_STAGES = $Stages }
        if ($Keys) { $env:EDEN_XLATE_KEYS = $Keys }
    }
    "== $t"
    & "$TestDir\run.ps1" -Tag $t -Script $Script -Seconds $Seconds -Shots $Shots -Profile $Profile
    Select-String -Path (Get-RunLog $t) -Pattern 'translator identity|time: virtual|validation|shader fail|cannot start' |
        Select-Object -First 6 | ForEach-Object { '  ' + $_.Line.Substring(0, [Math]::Min(170, $_.Line.Length)) }
}
foreach ($v in $vars) { Remove-Item "Env:$v" -ErrorAction SilentlyContinue }
