# Frames out of a video with Windows' own media stack (Windows.Media.Editing), nothing downloaded: one JPEG per
# requested second, at the given width. Usage: frames.ps1 -Video <mp4> -Out <dir> -Times 1,2.5,3 -Width 1280
param([string]$Video, [string]$Out, [double[]]$Times, [int]$Width = 1280)
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Media.Editing.MediaComposition, Windows.Media.Editing, ContentType = WindowsRuntime]
$null = [Windows.Media.Editing.MediaClip, Windows.Media.Editing, ContentType = WindowsRuntime]
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, [Type]$t) { $task = $asTask.MakeGenericMethod($t).Invoke($null, @($op)); $task.Wait(-1) | Out-Null; $task.Result }
New-Item -ItemType Directory -Force $Out | Out-Null
$file = Await ([Windows.Storage.StorageFile]::GetFileFromPathAsync($Video)) ([Windows.Storage.StorageFile])
$clip = Await ([Windows.Media.Editing.MediaClip]::CreateFromFileAsync($file)) ([Windows.Media.Editing.MediaClip])
$comp = New-Object Windows.Media.Editing.MediaComposition
try { $comp.Clips.Append($clip) } catch { [System.Collections.Generic.ICollection[Windows.Media.Editing.MediaClip]].GetMethod("Add").Invoke($comp.Clips, @($clip)) }
"duration {0:F1} s" -f $comp.Duration.TotalSeconds
if ($comp.Duration.TotalSeconds -le 0) { "no clip in the composition"; exit 1 }
$h = [int]($Width * 9 / 16)
foreach ($t in $Times) {
    $ts = [TimeSpan]::FromSeconds($t)
    $img = Await ($comp.GetThumbnailAsync($ts, $Width, $h, [Windows.Media.Editing.VideoFramePrecision]::NearestFrame)) ([Windows.Graphics.Imaging.ImageStream])
    $stream = [System.IO.WindowsRuntimeStreamExtensions]::AsStreamForRead($img)
    $path = Join-Path $Out ('t{0:000.00}.jpg' -f $t)
    $fs = [System.IO.File]::Create($path); $stream.CopyTo($fs); $fs.Close(); $stream.Close()
    $path
}
