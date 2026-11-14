// The window's three views (setup, building / stopped, done) over the builder's events (builder/events.py):
// start and plan give the steps, step and progress move them, error and end close the run. The window decides nothing
// about the build itself: the checks, the order and the resume rule are the engine's (builder/steps.py).
using System.Collections.ObjectModel;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Text.Json;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Media.Imaging;
using System.Windows.Threading;
using Microsoft.Win32;

namespace SrBuilder;

public class Row : INotifyPropertyChanged
{
    string state = "pending";
    public string Id { get; set; }
    public string Text { get; set; }
    public string State
    {
        get => state;
        set { state = value; PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(nameof(State))); }
    }
    public event PropertyChangedEventHandler PropertyChanged;
}

public partial class MainWindow : Window
{
    const double GiB = 1L << 30;
    readonly Options opt;
    readonly Settings settings = Settings.Load();
    readonly string repo, python;
    readonly Catalog text;
    readonly Engine engine = new();
    readonly ObservableCollection<Row> rows = new();
    readonly DispatcherTimer clock = new() { Interval = TimeSpan.FromSeconds(1) };
    readonly Stopwatch watch = new();
    readonly List<string> warnings = new();
    List<string> lastArgs;
    string logPath, errorText, restoreOut, runDest, runGame;
    bool restoring, ended, deleteAsked, originalRemoved;
    int shot;
    // the game file's check (python -m builder check-game): none, checking, ok or bad, for the path it was asked for
    readonly DispatcherTimer checkSoon = new() { Interval = TimeSpan.FromMilliseconds(350) };
    string gameState = "none", gameChecked = "", gameProblem;

    // what a step is known to take, from the run the builder was tested with
    static readonly Dictionary<string, string> StepNotes = new()
    {
        ["compile"] = "This is the longest step: about 5 minutes on the PC it was tested on. The PC may feel slow meanwhile.",
        ["shaders"] = "About 2 minutes on the PC it was tested on.",
    };

    public MainWindow(Options o)
    {
        opt = o;
        InitializeComponent();
        repo = FindRepo(o.Repo);
        var venv = repo == null ? null : Path.Combine(Path.GetDirectoryName(repo) ?? "", ".toolchains", "venv", "Scripts", "python.exe");
        var own = repo == null ? null : Path.Combine(Path.GetDirectoryName(repo) ?? "", "python", "python.exe");   // a package's
        python = venv != null && File.Exists(venv) ? venv : own != null && File.Exists(own) ? own : "python";
        text = new Catalog(repo);
        StepsList.ItemsSource = rows;
        SegList.ItemsSource = rows;
        GameBox.Text = o.Game ?? settings.Game;
        FirmwareBox.Text = o.Firmware ?? settings.Firmware;
        KeysBox.Text = o.Keys ?? settings.Keys;
        DestBox.Text = o.Dest ?? settings.Dest;
        DesktopBox.IsChecked = settings.Desktop;
        StartMenuBox.IsChecked = settings.StartMenu;
        DeleteBox.IsChecked = settings.DeleteOriginal;
        engine.Event += e => Dispatcher.BeginInvoke(() => OnEvent(e));
        engine.Exited += (code, err) => Dispatcher.BeginInvoke(() => OnExited(code, err));
        clock.Tick += (_, _) => Tick();
        checkSoon.Tick += (_, _) => { checkSoon.Stop(); CheckGame(); };
        Closing += (_, _) => engine.Cancel();
        RefreshSetup();
        AskTools();
        if (repo == null) SetupMessage.Text = "The builder's files were not found next to this window (--repo).";
        Loaded += (_, _) =>
        {
            if (o.Shots != null) Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, new Action(() => Samples(o.Shots)));
            else if (o.Go)
            {
                Build_Click(null, null);
                if (o.StopAfter > 0)
                {
                    var t = new DispatcherTimer { Interval = TimeSpan.FromSeconds(o.StopAfter) };
                    t.Tick += (_, _) => { t.Stop(); if (engine.Running) Stop_Click(null, null); };
                    t.Start();
                }
            }
        };
    }

    static string FindRepo(string given)
    {
        if (!string.IsNullOrEmpty(given)) return File.Exists(Path.Combine(given, "builder", "__main__.py")) ? Path.GetFullPath(given) : null;
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
        {
            if (File.Exists(Path.Combine(d.FullName, "builder", "__main__.py"))) return d.FullName;
            if (File.Exists(Path.Combine(d.FullName, "app", "builder", "__main__.py"))) return Path.Combine(d.FullName, "app");   // a package
            if (File.Exists(Path.Combine(d.FullName, "sr-recomp", "builder", "__main__.py"))) return Path.Combine(d.FullName, "sr-recomp");
        }
        return null;
    }

    // ---------------------------------------------------------------- setup
    void Paths_Changed(object sender, EventArgs e) { if (IsInitialized && DestInfo != null) RefreshSetup(); }

    void RefreshSetup()
    {
        SetupMessage.Text = "";
        var game = GameBox.Text.Trim();
        long size = 0;
        if (game != gameChecked)   // a new path: checked a moment after the typing stops
        {
            gameChecked = game;
            gameProblem = null;
            gameState = game == "" ? "none" : "checking";
            checkSoon.Stop();
            if (game != "") checkSoon.Start();
        }
        if (game != "" && File.Exists(game)) size = new FileInfo(game).Length;
        // a NUP cannot be made again exactly from the game folder, so it is never removed (steps.py Original)
        bool nup = game.EndsWith(".nup", StringComparison.OrdinalIgnoreCase);
        DeleteBox.IsEnabled = !nup;
        if (nup) DeleteBox.IsChecked = false;
        DeleteBox.Opacity = nup ? 0.5 : 1;
        DeleteNote.Text = nup ? "Not for a .nup: it cannot be made again exactly from the game folder, so it is always kept."
                              : "Only after checking that the folder rebuilds it exactly. It goes to the Recycle Bin.";
        ShowGameState(size);
        var dest = DestBox.Text.Trim();
        if (dest == "") { DestInfo.Text = " "; return; }
        if (File.Exists(Path.Combine(dest, "sr-build.json")))
        {
            DestInfo.Text = "This folder already holds a build: only the steps that changed run again.";
            return;
        }
        // the engine's own rule (steps.py Check): the game file's size plus 6 GiB here, 3 GiB of temporary space
        var need = size > 0 ? $"about {(size + 6 * GiB) / GiB:0} GiB" : "the game file's size plus 6 GiB";
        var free = "";
        try
        {
            var root = Path.GetPathRoot(Path.GetFullPath(dest));
            if (!string.IsNullOrEmpty(root) && Directory.Exists(root))
                free = $" Drive {root.TrimEnd('\\')} has {new DriveInfo(root).AvailableFreeSpace / GiB:0} GiB free.";
        }
        catch (Exception) { }
        DestInfo.Text = $"Needs {need} here and 3 GiB of temporary space.{free}";
    }

    // The game file's line, the box's edge and BUILD: a file that is not the game the port takes does not get past here.
    void ShowGameState(long size)
    {
        bool bad = gameState == "bad";
        GameBox.BorderBrush = (Brush)FindResource(bad ? "Orange" : "Line");
        GameInfo.Foreground = (Brush)FindResource(bad ? "Amber" : gameState == "ok" ? "Mint" : "Dim");
        GameInfo.Text = gameState switch
        {
            "ok" => $"The right game and version. {size / GiB:0.0} GiB.",
            "bad" => "Not valid: " + gameProblem,
            "checking" => "Checking the file…",
            _ => " ",
        };
        BuildButton.IsEnabled = gameState is "none" or "ok";
    }

    void CheckGame()
    {
        var path = gameChecked;
        if (path == "" || repo == null) { gameState = repo == null && path != "" ? "ok" : gameState; ShowGameState(0); return; }
        string problem = null;
        bool ok = false;
        var check = new Engine();
        check.Event += e => Dispatcher.BeginInvoke(() =>
        {
            var type = e.GetProperty("type").GetString();
            if (type == "error") problem = text.Get(e.GetProperty("id").GetString(), e.TryGetProperty("params", out var p) ? p : null);
            else if (type == "game") ok = true;
        });
        check.Exited += (code, err) => Dispatcher.BeginInvoke(() =>
        {
            if (path != gameChecked) return;   // the path changed meanwhile: that one's check answers
            gameState = ok && code == 0 ? "ok" : "bad";
            gameProblem = problem ?? "the file could not be checked.";
            long size = 0;
            try { if (File.Exists(path)) size = new FileInfo(path).Length; } catch (Exception) { }
            ShowGameState(size);
            if (SetupMessage.Text.StartsWith("Still checking"))
                SetupMessage.Text = gameState == "bad" ? "The game file is not valid: choose another." : "";
            LiveShot("game-" + gameState);
            if (pendingBuild) { pendingBuild = false; if (gameState == "ok") Build_Click(null, null); }
        });
        try { check.Start(python, repo, new[] { "check-game", "--game", path }); }
        catch (Exception x)
        {
            gameState = "bad";
            gameProblem = $"the check could not be run ({x.Message}).";
            ShowGameState(0);
        }
    }
    bool pendingBuild;   // --go asked for a build before the check answered

    // the tools and clones folders, when given (--toolchains, --vendor): added to the builds and to the tools question
    IEnumerable<string> Tc()
    {
        var a = new List<string>();
        if (opt.Toolchains != null) { a.Add("--toolchains"); a.Add(opt.Toolchains); }
        if (opt.Vendor != null) { a.Add("--vendor"); a.Add(opt.Vendor); }
        return a;
    }

    // What a first build would download (python -m builder tools --list): said on the setup, and the build is
    // then started with --fetch-tools, so the check step gets them.
    void AskTools()
    {
        if (repo == null) return;
        var ask = new Engine();
        ask.Event += e => Dispatcher.BeginInvoke(() =>
        {
            if (e.GetProperty("type").GetString() != "tools") return;
            double bytes = e.GetProperty("bytes").GetDouble();
            int n = e.GetProperty("missing").GetArrayLength();
            ToolsNote.Visibility = bytes > 0 || n > 0 ? Visibility.Visible : Visibility.Collapsed;
            ToolsNote.Text = bytes > 0 ? $"The first build also downloads {bytes / 1e6:0} MB of tools, each from its official source."
                : n > 0 ? "The first build also prepares its tools: a moment more." : "";
            LiveShot("tools");
        });
        try { ask.Start(python, repo, new[] { "tools", "--list" }.Concat(Tc())); } catch (Exception) { }
    }

    void BrowseGame_Click(object sender, RoutedEventArgs e)
    {
        var d = new OpenFileDialog { Title = "The game file", Filter = "Game file (*.nsp;*.nup)|*.nsp;*.nup|All files|*.*" };
        if (d.ShowDialog(this) == true) GameBox.Text = d.FileName;
    }

    void BrowseKeys_Click(object sender, RoutedEventArgs e)
    {
        var d = new OpenFileDialog { Title = "prod.keys", Filter = "Keys file (*.keys)|*.keys|All files|*.*" };
        if (d.ShowDialog(this) == true) KeysBox.Text = d.FileName;
    }

    void BrowseFirmware_Click(object sender, RoutedEventArgs e)
    {
        var d = new OpenFolderDialog { Title = "The firmware folder" };
        if (d.ShowDialog(this) == true) FirmwareBox.Text = d.FolderName;
    }

    void BrowseDest_Click(object sender, RoutedEventArgs e)
    {
        var d = new OpenFolderDialog { Title = "The game folder" };
        if (d.ShowDialog(this) == true) DestBox.Text = d.FolderName;
    }

    void Build_Click(object sender, RoutedEventArgs e)
    {
        string game = GameBox.Text.Trim(), fw = FirmwareBox.Text.Trim(), keys = KeysBox.Text.Trim(), dest = DestBox.Text.Trim();
        bool built = dest != "" && File.Exists(Path.Combine(dest, "sr-build.json"));
        if (gameState == "checking") { pendingBuild = sender == null; SetupMessage.Text = "Still checking the game file."; return; }
        if (gameState == "bad") { SetupMessage.Text = "The game file is not valid: choose another."; return; }
        string problem = repo == null ? "The builder's files were not found next to this window (--repo)."
            : dest == "" ? "Choose the game folder."
            : game == "" && !built ? text.Get("check.need_game")
            : game != "" && !File.Exists(game) ? text.Get("check.game_missing", ("path", game))
            : !built && (keys == "" || fw == "") ? text.Get("check.need_keys")
            : keys != "" && !File.Exists(keys) ? text.Get("check.keys_missing", ("path", keys))
            : fw != "" && !Directory.Exists(fw) ? $"The firmware folder {fw} does not exist."
            : null;
        if (problem != null) { SetupMessage.Text = problem; return; }
        settings.Game = game; settings.Firmware = fw; settings.Keys = keys; settings.Dest = dest;
        settings.Desktop = DesktopBox.IsChecked == true;
        settings.StartMenu = StartMenuBox.IsChecked == true;
        settings.DeleteOriginal = DeleteBox.IsChecked == true;
        settings.Save();
        var args = new List<string> { "--dest", dest, "--fetch-tools" };
        args.AddRange(Tc());
        if (game != "") { args.Add("--game"); args.Add(game); }
        if (keys != "") { args.Add("--keys"); args.Add(keys); }
        if (fw != "") { args.Add("--firmware"); args.Add(fw); }
        if (settings.Desktop) args.Add("--desktop-shortcut");
        if (settings.StartMenu) args.Add("--start-menu");
        if (settings.DeleteOriginal) args.Add("--delete-original");
        restoring = false;
        runDest = dest; runGame = game; deleteAsked = settings.DeleteOriginal;
        Run(args);
    }

    // ---------------------------------------------------------------- a run
    void Run(List<string> args)
    {
        lastArgs = args;
        ended = false; errorText = null; logPath = null; originalRemoved = false;
        warnings.Clear();
        rows.Clear();
        if (restoring) rows.Add(new Row { Id = "restore", Text = text.Get("phase.restore.write"), State = "now" });
        ShowBuild(running: true);
        StepCount.Text = restoring ? "RESTORING" : "STARTING";
        StepTitle.Text = restoring ? text.Get("phase.restore.write") : "Getting ready";
        HeaderNote.Text = restoring ? $"Restoring to {restoreOut}" : $"Building into {runDest}";
        SetProgress(null, null, null);
        PhaseText.Text = "";
        NoteBoard.Visibility = Visibility.Collapsed;
        watch.Restart();
        clock.Start();
        Tick();
        try { engine.Start(python, repo, args); }
        catch (Exception x)
        {
            errorText = $"The builder could not be started ({x.Message}).";
            Finish("failed");
        }
    }

    void OnEvent(JsonElement e)
    {
        string S(string name) => e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
        switch (S("type"))
        {
            case "start":
                logPath = S("log");
                rows.Clear();
                foreach (var s in e.GetProperty("steps").EnumerateArray())
                {
                    // the engine always lists the step that removes the original; it does nothing unless asked
                    if (s.GetString() == "original" && !deleteAsked) continue;
                    rows.Add(new Row { Id = s.GetString(), Text = text.Get("step." + s.GetString()) });
                }
                break;
            case "plan":
                if (e.TryGetProperty("skip", out var skip) && skip.ValueKind == JsonValueKind.Object)
                    foreach (var p in skip.EnumerateObject()) Skipped(p.Name, p.Value.GetString());
                break;
            case "step":
                var row = rows.FirstOrDefault(r => r.Id == S("step"));
                switch (S("state"))
                {
                    case "started":
                        if (row == null) break;
                        row.State = "now";
                        StepCount.Text = $"STEP {rows.IndexOf(row) + 1} OF {rows.Count}";
                        StepTitle.Text = row.Text;
                        PhaseText.Text = "";
                        SetProgress(null, null, null);
                        if (StepNotes.TryGetValue(row.Id, out var note)) ShowNote(note);
                        else if (warnings.Count == 0) NoteBoard.Visibility = Visibility.Collapsed;
                        LiveShot(row.Id);
                        break;
                    case "done":
                        if (row != null) row.State = "done";
                        if (S("step") == "original" && deleteAsked) originalRemoved = true;
                        break;
                    case "skipped":
                        Skipped(S("step"), S("reason"));
                        break;
                }
                break;
            case "progress":
                PhaseText.Text = text.Get($"phase.{S("step")}.{S("phase")}") + (S("detail") is string what ? " " + what : "");
                double? done = e.TryGetProperty("done", out var d) ? d.GetDouble() : null;
                double? total = e.TryGetProperty("total", out var t) ? t.GetDouble() : null;
                SetProgress(done, total, S("unit"));
                break;
            case "warning":
                var w = text.Get(S("id"), e.TryGetProperty("params", out var wp) ? wp : null);
                warnings.Add(w);
                ShowNote(w);
                break;
            case "error":
                errorText = text.Get(S("id"), e.TryGetProperty("params", out var ep) ? ep : null);
                break;
            case "end":
                Finish(S("result"));
                break;
        }
    }

    void Skipped(string id, string reason)
    {
        var row = rows.FirstOrDefault(r => r.Id == id);
        if (row == null) return;
        if (reason == "not_needed") rows.Remove(row);   // a step this run does not have (the original stays, say)
        else row.State = "done";                         // finished by an earlier run
    }

    void OnExited(int code, string err)
    {
        if (ended) return;
        errorText ??= code == 3 ? null : "The builder stopped without a result." + (err == "" ? "" : "\n" + err);
        Finish(code == 3 ? "cancelled" : "failed");
    }

    void Finish(string result)
    {
        if (ended) return;
        ended = true;
        clock.Stop();
        watch.Stop();
        if (result == "ok") ShowDone();
        else ShowStopped(result == "cancelled");
        LiveShot("end");
    }

    void SetProgress(double? done, double? total, string unit)
    {
        FillBar.BeginAnimation(OpacityProperty, null);
        if (total is double t && t > 0 && done is double d)
        {
            var p = Math.Clamp(d / t, 0, 1);
            FillCol.Width = new GridLength(p * 100, GridUnitType.Star);
            RestCol.Width = new GridLength(100 - p * 100, GridUnitType.Star);
            FillBar.Opacity = 1;
            Drops.Visibility = p > 0.02 && p < 0.97 ? Visibility.Visible : Visibility.Hidden;
            PercentText.Text = p >= 0.12 ? $"{p * 100:0}%" : "";
            var amount = unit == "bytes" ? $"{d / GiB:0.0} of {t / GiB:0.0} GiB" : $"{d:0} of {t:0} {unit ?? ""}".TrimEnd();
            AmountText.Text = p < 0.12 ? $"{p * 100:0}%  ·  {amount}" : amount;
            return;
        }
        // no amount known: the whole bar, breathing
        FillCol.Width = new GridLength(100, GridUnitType.Star);
        RestCol.Width = new GridLength(0, GridUnitType.Star);
        Drops.Visibility = Visibility.Hidden;
        PercentText.Text = "";
        AmountText.Text = "";
        FillBar.BeginAnimation(OpacityProperty, new DoubleAnimation(0.25, 0.6, TimeSpan.FromSeconds(0.9))
            { AutoReverse = true, RepeatBehavior = RepeatBehavior.Forever });
    }

    void ShowNote(string s)
    {
        BuildNote.Text = s;
        NoteBoard.Visibility = Visibility.Visible;
    }

    void Tick()
    {
        var s = watch.Elapsed;
        ElapsedText.Text = $"{(int)s.TotalMinutes}:{s.Seconds:00} so far";
    }

    static string Clock(TimeSpan s) => $"{(int)s.TotalMinutes}:{s.Seconds:00}";

    // ---------------------------------------------------------------- views
    void ShowView(FrameworkElement view)
    {
        foreach (var v in new FrameworkElement[] { SetupView, BuildView, DoneView })
            v.Visibility = v == view ? Visibility.Visible : Visibility.Collapsed;
    }

    void ShowBuild(bool running)
    {
        ShowView(BuildView);
        RunningFooter.Visibility = running ? Visibility.Visible : Visibility.Collapsed;
        StoppedFooter.Visibility = running ? Visibility.Collapsed : Visibility.Visible;
        ProgressPanel.Visibility = running ? Visibility.Visible : Visibility.Collapsed;
        ErrorBoard.Visibility = running ? Visibility.Collapsed : Visibility.Visible;
        StopButton.IsEnabled = true;
        StopButton.Content = "STOP";
    }

    void ShowStopped(bool cancelled)
    {
        ShowBuild(running: false);
        FillBar.BeginAnimation(OpacityProperty, null);
        var row = rows.FirstOrDefault(r => r.State == "now");
        var done = rows.Count(r => r.State == "done");
        if (row != null)
        {
            row.State = "failed";
            StepCount.Text = restoring ? "STOPPED" : $"STOPPED AT STEP {rows.IndexOf(row) + 1} OF {rows.Count}";
            StepTitle.Text = row.Text;
        }
        else
        {
            StepCount.Text = "STOPPED";
            StepTitle.Text = restoring ? text.Get("phase.restore.write") : "The build did not start";
        }
        ErrorHead.Text = cancelled ? "STOPPED BY YOU" : "WHAT WENT WRONG";
        ErrorText.Text = cancelled ? "The build was stopped before this step finished." : errorText ?? "The builder gave no reason. The log has the details.";
        ShowNote(restoring ? "Nothing was changed in the game folder."
            : done == 0 ? "Nothing was built yet. Trying again starts from the first step."
            : $"Nothing is lost. The {Count(done)} finished step{(done == 1 ? " is" : "s are")} kept, and trying again starts at this one.");
        HeaderNote.Text = $"Stopped after {Clock(watch.Elapsed)}";
    }

    static string Count(int n) => n switch
    {
        1 => "one", 2 => "two", 3 => "three", 4 => "four", 5 => "five", 6 => "six", 7 => "seven", 8 => "eight",
        9 => "nine", 10 => "ten", 11 => "eleven", 12 => "twelve", _ => n.ToString(),
    };

    void ShowDone()
    {
        ShowView(DoneView);
        if (restoring)
        {
            HeaderNote.Text = $"Restored in {Clock(watch.Elapsed)}";
            DoneTitle.Text = "ORIGINAL RESTORED";
            (L1.Text, V1.Text) = ("FILE", restoreOut);
            (L2.Text, V2.Text) = ("CHECKED", "It matches the original exactly: same SHA-256 as when the folder was built.");
            (L3.Text, V3.Text) = ("FROM", runDest);
            PlayButton.Visibility = Visibility.Collapsed;
            DoneRestoreLink.Visibility = Visibility.Collapsed;
            DoneNote.Text = "";
            return;
        }
        HeaderNote.Text = $"Built in {Clock(watch.Elapsed)}";
        DoneTitle.Text = "READY TO PLAY";
        PlayButton.Visibility = Visibility.Visible;
        DoneRestoreLink.Visibility = Visibility.Visible;
        (L1.Text, V1.Text) = ("GAME FOLDER", $"{runDest}  ·  move it wherever you like");
        bool desk = settings.Desktop, start = settings.StartMenu;
        (L2.Text, V2.Text) = ("SHORTCUTS", desk && start ? "On the desktop and in the Start menu" : desk ? "On the desktop"
                                           : start ? "In the Start menu" : "None asked for");
        (L3.Text, V3.Text) = ("ORIGINAL FILE", originalRemoved ? "In the Recycle Bin  ·  this folder can rebuild it exactly"
            : string.IsNullOrEmpty(runGame) ? "Not touched" : $"Still at {runGame}  ·  the game no longer needs it");
        DoneNote.Text = "After a change to the port, run the builder on the same folder: only the steps that changed run again."
                        + (warnings.Count == 0 ? "" : "\nNote: " + string.Join(" ", warnings));
        var dest = runDest;
        Task.Run(() =>
        {
            try
            {
                long n = new DirectoryInfo(dest).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length);
                Dispatcher.BeginInvoke(() => { if (runDest == dest && !restoring) V1.Text = $"{dest}  ·  {n / GiB:0.0} GiB  ·  move it wherever you like"; });
            }
            catch (Exception) { }
        });
    }

    // ---------------------------------------------------------------- buttons
    void Stop_Click(object sender, RoutedEventArgs e)
    {
        StopButton.IsEnabled = false;
        StopButton.Content = "STOPPING";
        engine.Cancel();
    }

    void Retry_Click(object sender, RoutedEventArgs e) { if (lastArgs != null) Run(lastArgs); }

    void BackToSetup_Click(object sender, RoutedEventArgs e)
    {
        HeaderNote.Text = "Your own game file, made into a folder that plays on this PC";
        ShowView(SetupView);
        RefreshSetup();
    }

    void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        if (logPath != null && File.Exists(logPath)) Open(logPath);
        else if (logPath != null && Directory.Exists(Path.GetDirectoryName(logPath))) Open(Path.GetDirectoryName(logPath));
    }

    void OpenFolder_Click(object sender, RoutedEventArgs e) => Open(restoring ? Path.GetDirectoryName(restoreOut) : runDest);

    void Play_Click(object sender, RoutedEventArgs e)
    {
        var exe = Path.Combine(runDest ?? "", "sr.exe");
        if (!File.Exists(exe)) return;
        try { Process.Start(new ProcessStartInfo(exe) { WorkingDirectory = runDest, UseShellExecute = true }); Close(); }
        catch (Exception) { }
    }

    static void Open(string path)
    {
        try { Process.Start(new ProcessStartInfo(path) { UseShellExecute = true }); } catch (Exception) { }
    }

    void Restore_Click(object sender, RoutedEventArgs e)
    {
        var d = new OpenFolderDialog { Title = "The game folder to restore the original from" };
        if (Directory.Exists(DestBox.Text.Trim())) d.InitialDirectory = DestBox.Text.Trim();
        if (d.ShowDialog(this) == true) Restore(d.FolderName);
    }

    void RestoreFromDone_Click(object sender, RoutedEventArgs e) => Restore(runDest);

    void Restore(string folder)
    {
        if (repo == null) return;
        var s = new SaveFileDialog { Title = "Where to write the original game file", Filter = "Game file (*.nsp)|*.nsp", FileName = "game.nsp" };
        if (s.ShowDialog(this) != true) return;
        restoring = true;
        runDest = folder; restoreOut = s.FileName;
        Run(new List<string> { "restore", "--dest", folder, "--out", s.FileName });
    }

    void Minimize_Click(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;
    void Close_Click(object sender, RoutedEventArgs e) => Close();

    // ---------------------------------------------------------------- pictures (--shots, --live-shots)
    void Save(string path)
    {
        Root.UpdateLayout();
        var bmp = new RenderTargetBitmap(960, 640, 96, 96, PixelFormats.Pbgra32);
        bmp.Render(Root);
        var enc = new PngBitmapEncoder();
        enc.Frames.Add(BitmapFrame.Create(bmp));
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path)));
        using var f = File.Create(path);
        enc.Save(f);
    }

    void LiveShot(string name)
    {
        if (opt.LiveShots == null) return;
        var path = Path.Combine(opt.LiveShots, $"{++shot:00}-{name}.png");
        Dispatcher.BeginInvoke(DispatcherPriority.ApplicationIdle, new Action(() => { try { Save(path); } catch (Exception) { } }));
    }

    // each screen with sample values, for a look check without a build
    void Samples(string dir)
    {
        string[] ids = { "check", "romfs", "exefs", "lift", "compile", "fonts", "link", "dotnet", "shaders", "shortcut", "cleanup" };
        void Rows(int now, string state)
        {
            rows.Clear();
            for (int i = 0; i < ids.Length; i++)
                rows.Add(new Row { Id = ids[i], Text = text.Get("step." + ids[i]), State = i < now ? "done" : i == now ? state : "pending" });
        }
        GameBox.Text = @"D:\Dumps\game.nsp"; FirmwareBox.Text = @"D:\Dumps\Firmware"; KeysBox.Text = @"D:\Dumps\prod.keys";
        DestBox.Text = @"D:\Games\SR";
        checkSoon.Stop();
        gameState = "ok";
        ShowGameState((long)(12.8 * GiB));
        DestInfo.Text = "Needs about 15 GiB here and 3 GiB of temporary space. Drive D: has 120 GiB free.";
        SetupMessage.Text = "";
        ShowView(SetupView);
        Save(Path.Combine(dir, "1-setup.png"));
        gameState = "bad";
        gameProblem = text.Get("check.wrong_version", ("name", "game.nsp"), ("version", "131072"), ("expected", "65536"));
        ShowGameState(0);
        SetupMessage.Text = "The game file is not valid: choose another.";
        Save(Path.Combine(dir, "1b-setup-invalid.png"));
        SetupMessage.Text = "";

        runDest = @"D:\Games\SR"; runGame = @"D:\Dumps\game.nsp";
        Rows(4, "now");
        ShowBuild(running: true);
        HeaderNote.Text = $"Building into {runDest}";
        StepCount.Text = "STEP 5 OF 11";
        StepTitle.Text = text.Get("step.compile");
        PhaseText.Text = text.Get("phase.compile.compile");
        SetProgress(1196, 1930, "files");
        ElapsedText.Text = "3:10 so far";
        ShowNote(StepNotes["compile"]);
        Save(Path.Combine(dir, "2-building.png"));

        ShowView(DoneView);
        HeaderNote.Text = "Built in 7:12";
        (L1.Text, V1.Text) = ("GAME FOLDER", $"{runDest}  ·  15.0 GiB  ·  move it wherever you like");
        (L2.Text, V2.Text) = ("SHORTCUTS", "On the desktop and in the Start menu");
        (L3.Text, V3.Text) = ("ORIGINAL FILE", $"Still at {runGame}  ·  the game no longer needs it");
        DoneNote.Text = "After a change to the port, run the builder on the same folder: only the steps that changed run again.";
        Save(Path.Combine(dir, "3-done.png"));

        Rows(8, "now");
        errorText = text.Get("tool.failed", ("tool", "sr-xlate"), ("code", "1"), ("log", @"D:\SR_RECOMP\out\builder-temp\SR-1a2b3c4d\logs\shaders.log"));
        watch.Reset();
        ShowStopped(cancelled: false);
        HeaderNote.Text = "Stopped after 6:40";
        Save(Path.Combine(dir, "4-stopped.png"));
        Application.Current.Shutdown();
    }
}
