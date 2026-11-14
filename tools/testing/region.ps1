# offset.ps1 for one rectangle of the picture (x0,y0 to x1,y1 in the pictures' pixels): whether one object follows the
# same timeline as the rest (an idle animation drawn at random does not match at any offset).
#   region.ps1 -A a1 -B a2 -X0 440 -Y0 140 -X1 480 -Y1 235 [-W 40]
param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B, [int]$X0, [int]$Y0, [int]$X1, [int]$Y1, [int]$W = 12)
. "$PSScriptRoot\common.ps1"
$fa = Get-Frames $A; $fb = Get-Frames $B
"$A vs $B, region $X0,$Y0 to $X1,$Y1"
foreach ($n in ($fa.Keys | Sort-Object)) {
    if ($n % 20) { continue }
    $width = 0
    $a = [SrTest.Img]::Load($fa[$n], [ref]$width); $best = 1e9; $bo = 0; $at = @{}
    for ($o = -$W; $o -le $W; $o++) {
        if (-not $fb.ContainsKey($n + $o)) { continue }
        $d = [SrTest.Img]::Diff($a, [SrTest.Img]::Load($fb[$n + $o]), $width, $X0, $Y0, $X1, $Y1)
        if ($o -eq 0) { $at[0] = $d }
        if ($d -lt $best) { $best = $d; $bo = $o }
    }
    '{0}: best {1:+0;-0;0} ({2:N1}), same frame {3:N1}' -f $n, $bo, $best, $at[0]
}
