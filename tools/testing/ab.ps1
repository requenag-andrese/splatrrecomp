# A/B within one run: the switches under test take "ab" (SR_AB=1 flips them in blocks A B B A of 1 s of world clock, or
# of frames when the game has no world clock), every frame of a window is captured and compared with the one before. A
# change the switches make shows as a jump where the log says a block began; time and camera motion move every frame a
# little. State that lasts (the ink map) needs separate runs instead (runs.ps1).
#   ab.ps1 -Tag ab1 -Env "SR_UB_REACH=ab" [-Script still] [-Shots 1900-2260:1]
param([Parameter(Mandatory)][string]$Tag, [string]$Env = '', [string]$Script = 'still', [string]$Shots = '1900-2260:1',
      [int]$Seconds = 45, [switch]$Full)
. "$PSScriptRoot\common.ps1"
if (Test-GameOpen) { 'sr.exe is open: no test'; exit 2 }
$set = @('SR_AB')
$env:SR_AB = '1'
foreach ($kv in ($Env -split ' ' | Where-Object { $_ })) { $k, $v = $kv -split '=', 2; Set-Item "Env:$k" $v; $set += $k }
& "$TestDir\run.ps1" -Tag $Tag -Script $Script -Seconds $Seconds -Shots $Shots -Full:$Full | Out-Null
foreach ($k in $set) { Remove-Item "Env:$k" -ErrorAction SilentlyContinue }
$marks = @{}
Select-String -Path (Get-RunLog $Tag) -Pattern 'ab: (A|B) from frame ([0-9]+)' |
    ForEach-Object { $marks[[int]$_.Matches[0].Groups[2].Value] = $_.Matches[0].Groups[1].Value }
$frames = Get-Frames $Tag
$prev = $null; $pn = -1; $rows = @()
foreach ($n in ($frames.Keys | Sort-Object)) {
    $cur = [SrTest.Img]::Load($frames[$n])
    if ($prev -and $n -eq $pn + 1) { $rows += [pscustomobject]@{ n = $n; d = [SrTest.Img]::Diff($prev, $cur) } }
    $prev = $cur; $pn = $n
}
"  frames compared $($rows.Count), mean change {0:F2}" -f ($rows | Measure-Object d -Average).Average
foreach ($m in ($marks.Keys | Sort-Object)) {
    $near = $rows | Where-Object { $_.n -ge $m - 3 -and $_.n -le $m + 3 }
    if ($near) { "  block $($marks[$m]) from $m : " + (($near | ForEach-Object { '{0}:{1:F2}' -f $_.n, $_.d }) -join '  ') }
}
