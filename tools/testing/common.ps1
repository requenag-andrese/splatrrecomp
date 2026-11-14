# Shared by the test scripts in this folder (dot-sourced): where things are, closing the game window, comparing pictures.
# Paths default to the usual workspace layout around the repository and can be moved with environment variables:
#   SR_TEST_BUILD     the build that runs (default <workspace>/out/build-vk, sr.exe inside)
#   SR_TEST_RUNS      run outputs: pictures, logs, per-run settings and profile copies (default <workspace>/out/test-runs)
#   SR_TEST_PROFILES  base profiles the runs copy (default <workspace>/out/test-profiles); "hub" is a save in the hub,
#                     "start" a fresh one. Saves are game data: they stay outside the repository.
$TestDir = $PSScriptRoot
$Repo = (Resolve-Path (Join-Path $TestDir '..\..')).Path
$Workspace = Split-Path $Repo -Parent
$Build = if ($env:SR_TEST_BUILD) { $env:SR_TEST_BUILD } else { Join-Path $Workspace 'out\build-vk' }
$Runs = if ($env:SR_TEST_RUNS) { $env:SR_TEST_RUNS } else { Join-Path $Workspace 'out\test-runs' }
$Profiles = if ($env:SR_TEST_PROFILES) { $env:SR_TEST_PROFILES } else { Join-Path $Workspace 'out\test-profiles' }
New-Item -ItemType Directory -Force $Runs | Out-Null

if (-not ('SrTest.Win' -as [type])) {
    Add-Type -Namespace SrTest -Name Win -MemberDefinition '[DllImport("user32.dll")] public static extern bool PostMessageW(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);'
}

# 1 when a game is open (a test never starts next to one: the user may be playing).
function Test-GameOpen { [bool](Get-Process sr -ErrorAction SilentlyContinue) }

# Closes the test instance the way its close button does, again every 10 s while it stays, for up to $Seconds.
function Close-Game([int]$Seconds = 90) {
    $w = Get-Date
    while (($p = Get-Process sr -ErrorAction SilentlyContinue) -and ((Get-Date) - $w).TotalSeconds -lt $Seconds) {
        foreach ($q in @($p)) { if ($q.MainWindowHandle -ne 0) { [void][SrTest.Win]::PostMessageW($q.MainWindowHandle, 0x10, [IntPtr]0, [IntPtr]0) } }
        Start-Sleep -Seconds 10
    }
}

# The run folder of a tag (pictures f<frame>.png) and its log.
function Get-RunDir([string]$Tag) { Join-Path $Runs $Tag }
function Get-RunLog([string]$Tag) { Join-Path $Runs "$Tag.err" }

# Picture comparison: mean per-pixel difference (largest channel, 0..255), whole picture or a rectangle, and a
# side-by-side difference picture (left the first picture, right the difference x8).
if (-not ('SrTest.Img' -as [type])) {
    Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @"
using System; using System.Drawing; using System.Drawing.Imaging; using System.Runtime.InteropServices;
namespace SrTest {
public static class Img {
 public static byte[] Load(string p, out int w){ using(var b=new Bitmap(p)){ w=b.Width; var d=b.LockBits(new Rectangle(0,0,b.Width,b.Height),ImageLockMode.ReadOnly,PixelFormat.Format32bppArgb); var a=new byte[b.Width*b.Height*4]; Marshal.Copy(d.Scan0,a,0,a.Length); b.UnlockBits(d); return a; } }
 public static byte[] Load(string p){ int w; return Load(p, out w); }
 public static double Diff(byte[] a, byte[] b){ if(a.Length!=b.Length) return -1; double s=0; for(int i=0;i<a.Length;i+=4){ int d=0; for(int k=0;k<3;k++) d=Math.Max(d,Math.Abs(a[i+k]-b[i+k])); s+=d; } return s/(a.Length/4); }
 public static double Diff(byte[] a, byte[] b, int width, int x0, int y0, int x1, int y1){ double s=0; int n=0; for(int y=y0;y<y1;y++) for(int x=x0;x<x1;x++){ int i=(y*width+x)*4; if(i+3>=a.Length||i+3>=b.Length) continue; int d=0; for(int k=0;k<3;k++) d=Math.Max(d,Math.Abs(a[i+k]-b[i+k])); s+=d; n++; } return n>0?s/n:-1; }
 // For every step-th frame f<n>.png of folder a, the frame of folder b within +-w that matches best (offset.ps1)
 public static string Offset(string da, string db, int w, int step){
  var bs=new System.Collections.Generic.Dictionary<int,byte[]>();
  foreach(var f in System.IO.Directory.GetFiles(db,"f*.png")) bs[int.Parse(System.IO.Path.GetFileNameWithoutExtension(f).Substring(1))]=Load(f);
  var sb=new System.Text.StringBuilder(); var fa=System.IO.Directory.GetFiles(da,"f*.png"); Array.Sort(fa);
  foreach(var f in fa){ int n=int.Parse(System.IO.Path.GetFileNameWithoutExtension(f).Substring(1)); if(n%step!=0) continue; var a=Load(f);
   double best=1e9, same=-1; int bo=0;
   for(int o=-w;o<=w;o++){ byte[] b; if(!bs.TryGetValue(n+o,out b)) continue; double d=Diff(a,b); if(o==0) same=d; if(d<best){best=d;bo=o;} }
   sb.AppendFormat("{0}: same {1:F2}, best {2:+0;-0;0} ({3:F2})\n", n, same, bo, best); }
  return sb.ToString(); }
 public static string DiffPicture(string pa, string pb, string outp){
  using(var a=new Bitmap(pa)) using(var b=new Bitmap(pb)){
   int w=a.Width,h=a.Height; var o=new Bitmap(w*2,h); long big=0;
   using(var g=Graphics.FromImage(o)) g.DrawImage(a,0,0,w,h);
   for(int y=0;y<h;y++) for(int x=0;x<w;x++){ var p=a.GetPixel(x,y); var q=b.GetPixel(x,y);
    int d=Math.Max(Math.Abs(p.R-q.R),Math.Max(Math.Abs(p.G-q.G),Math.Abs(p.B-q.B))); if(d>24) big++;
    int v=Math.Min(255,d*8); o.SetPixel(w+x,y,Color.FromArgb(v,v,v)); }
   o.Save(outp); return w+"x"+h+", pixels off by more than 24: "+(100.0*big/(w*h)).ToString("F2")+"%";
  } }
}}
"@
}

# The frames of a run folder as {number -> path}.
function Get-Frames([string]$Tag) {
    $m = @{}
    foreach ($f in Get-ChildItem (Get-RunDir $Tag) -Filter 'f*.png' -ErrorAction SilentlyContinue) { $m[[int]$f.BaseName.Substring(1)] = $f.FullName }
    $m
}
