# (tools/testing) Output: <runs>/threads-<date>-<time>.txt unless -Out. Start it, then play.
# Every few seconds: how much CPU each named thread of sr.exe used, and the whole machine's load. Reads Windows'
# counters only (OpenThread with query rights, GetThreadDescription); the game is not touched.
param([int]$Minutes = 15, [int]$Every = 5, [string]$Out = '')
. "$PSScriptRoot\common.ps1"
if (-not $Out) { $Out = Join-Path $Runs "threads-$(Get-Date -Format yyyyMMdd-HHmmss).txt" }
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class TD {
  [DllImport("kernel32.dll")] static extern IntPtr OpenThread(uint access, bool inherit, uint id);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll")] static extern int GetThreadDescription(IntPtr h, out IntPtr desc);
  [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr p);
  public static string Name(int id) {
    IntPtr h = OpenThread(0x0800, false, (uint)id);   // THREAD_QUERY_LIMITED_INFORMATION
    if (h == IntPtr.Zero) return "";
    IntPtr p; string s = "";
    if (GetThreadDescription(h, out p) >= 0 && p != IntPtr.Zero) { s = Marshal.PtrToStringUni(p); LocalFree(p); }
    CloseHandle(h); return s;
  }
}
"@
$cores = [Environment]::ProcessorCount
$names = @{}
function Snap {
    $p = Get-Process sr -ErrorAction SilentlyContinue
    if (-not $p) { return $null }
    $t = @{}
    foreach ($th in $p.Threads) {
        try { $t[$th.Id] = $th.TotalProcessorTime.TotalMilliseconds } catch {}
        if (-not $names.ContainsKey($th.Id)) { $n = [TD]::Name($th.Id); $names[$th.Id] = $(if ($n) { $n } else { "tid$($th.Id)" }) }
    }
    $t
}
"started $(Get-Date -Format HH:mm:ss), $cores logical processors" | Set-Content $Out
$end = (Get-Date).AddMinutes($Minutes)
$prev = Snap; $pt = Get-Date
$load = New-Object System.Diagnostics.PerformanceCounter('Processor', '% Processor Time', '_Total'); [void]$load.NextValue()
while ((Get-Date) -lt $end) {
    Start-Sleep -Seconds $Every
    $cur = Snap; $now = Get-Date
    if (-not $cur) { "$(Get-Date -Format HH:mm:ss) sr.exe not running" | Add-Content $Out; $prev = $null; continue }
    $ms = ($now - $pt).TotalMilliseconds
    $machine = $load.NextValue()
    # group threads by name (numbered workers fold together: 'guest core 3' stays itself)
    $by = @{}
    foreach ($id in $cur.Keys) {
        if ($prev -and $prev.ContainsKey($id)) {
            $d = $cur[$id] - $prev[$id]; $n = $names[$id]
            if ($by.ContainsKey($n)) { $by[$n] += $d } else { $by[$n] = $d }
        }
    }
    $total = ($by.Values | Measure-Object -Sum).Sum
    $top = $by.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 10 | ForEach-Object { '{0} {1:N0}%' -f $_.Key, (100 * $_.Value / $ms) }
    ('{0} machine {1:N0}% | sr {2:N0}% of one core ({3:N1} cores) | {4}' -f $now.ToString('HH:mm:ss'), $machine, (100 * $total / $ms), ($total / $ms), ($top -join ', ')) | Add-Content $Out
    $prev = $cur; $pt = $now
}
"ended $(Get-Date -Format HH:mm:ss)" | Add-Content $Out
