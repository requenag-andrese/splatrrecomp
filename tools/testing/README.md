# Test tools

Scripts the runtime's changes are checked with: game runs with an input script and a picture series, comparisons
between runs, A/B inside one run, translator comparisons. Windows PowerShell 5.1 for the `.ps1`, the workspace's venv
for the `.py` (`..\..\..\.toolchains\venv\Scripts\python.exe -I`).

Nothing of the game goes into the repository: runs write to `<workspace>/out/test-runs` and copy base profiles (saves)
from `<workspace>/out/test-profiles` (`hub`: a save in the hub; `start`: a fresh one). `common.ps1` has the paths and
the variables that move them (`SR_TEST_BUILD`, `SR_TEST_RUNS`, `SR_TEST_PROFILES`). Every runner refuses to start while
an `sr.exe` is open, and closes only the instance it started.

| Script | What it does |
| --- | --- |
| `run.ps1` | One run: profile copy, muted, an input script, pictures `f<frame>.png`, the log `<tag>.err`; closes the window at the end. |
| `runs.ps1` | Comparable runs: virtual time (`SR_VIRTUAL_TIME=0`), a fresh profile each; tags starting with `ed` use eden-xlate (translator study). |
| `offset.ps1` | Two runs' frames aligned: the load-length offset, and the difference once aligned (what a change made). |
| `compare.ps1` | Mean difference of the same frame numbers. |
| `region.ps1` | `offset.ps1` for one rectangle (does an object follow the same timeline). |
| `diff-image.ps1` | A picture and its difference x8 with another run's frame. |
| `ab.ps1` | A/B inside one run (`SR_AB=1` and switches set to `ab`): frame-to-frame jumps where the blocks change. |
| `threads.ps1` | CPU per named thread of a running `sr.exe` every few seconds (Windows counters only). |
| `frames.ps1` | Frames out of a video (Windows.Media.Editing), for clips the user records. |
| `build-vk.sh` | The usual build into `out/build-vk` (log in `out/build-final-vk.log`). |
| `input/` | Input scripts (`SR_INPUT_SCRIPT` lines: `<frame> <button> [hold]`, `<frame> LSTICK x y [hold]`). From the hub save: `still` stands, `pan` turns the camera, `ink` and `ink2` paint and walk. |
| `xlate/` | Translator study: batch jobs from the recorded programs, both translators' outputs compared (bindings, interface, validity). |

Typical checks:

```
# does a change alter the picture? two runs before / after, aligned
.\runs.ps1 -Tags before -Script ink                   # with the old build
.\runs.ps1 -Tags after -Script ink                    # with the new one
.\offset.ps1 -A before -B after
.\diff-image.ps1 -A before -FA 1200 -B after -FB 1197

# two runs of the same build differ by ~0.5-1.0/255 once aligned (render noise, random idle animations)
```

Lessons kept here: virtual time makes runs comparable only after aligning the load offset; state that persists across
frames (the ink map) needs separate runs, not `ab.ps1`; never pipe a runner into `Select-Object -First`, which stops it
before it closes the game.
