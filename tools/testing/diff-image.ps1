# A difference picture of frame FA of run A against frame FB of run B (default the same number): left the first
# picture, right the difference x8, and the share of pixels off by more than 24.
#   diff-image.ps1 -A a1 -FA 1200 -B a2 [-FB 1197] [-Out <png>]
param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][int]$FA, [Parameter(Mandatory)][string]$B, [int]$FB = -1, [string]$Out)
. "$PSScriptRoot\common.ps1"
if ($FB -lt 0) { $FB = $FA }
$pa = Join-Path (Get-RunDir $A) ('f{0:D5}.png' -f $FA)
$pb = Join-Path (Get-RunDir $B) ('f{0:D5}.png' -f $FB)
if (-not $Out) { $Out = Join-Path $Runs ('diff-{0}-{1}-{2}-{3}.png' -f $A, $FA, $B, $FB) }
[SrTest.Img]::DiffPicture($pa, $pb, $Out)
$Out
