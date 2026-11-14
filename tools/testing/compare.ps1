# Mean per-pixel difference (largest channel, 0..255) between the same frame numbers of two runs.
#   compare.ps1 -A a1 -B a2
param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B)
. "$PSScriptRoot\common.ps1"
$fa = Get-Frames $A; $fb = Get-Frames $B
$out = foreach ($n in ($fa.Keys | Sort-Object)) {
    if ($fb.ContainsKey($n)) { 'f{0:D5} {1:N2}' -f $n, [SrTest.Img]::Diff([SrTest.Img]::Load($fa[$n]), [SrTest.Img]::Load($fb[$n])) }
}
"$A vs $B : " + ($out -join ' | ')
