# For every 10th frame of run A, the frame of run B within +-W that matches best, and the difference there: two runs
# under virtual time differ by a constant offset (how long each load took), and the aligned difference is what a
# change made. Capture every frame of a window (-Shots 1100-1300:1) in both runs.
#   offset.ps1 -A a1 -B a2 [-W 12]
param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B, [int]$W = 12)
. "$PSScriptRoot\common.ps1"
"$A vs $B"
[SrTest.Img]::Offset((Get-RunDir $A), (Get-RunDir $B), $W, 10)
