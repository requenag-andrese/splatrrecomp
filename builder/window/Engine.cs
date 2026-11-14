// Runs `python -m builder ...` and hands over its JSON events, one per stdout line (builder/events.py).
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace SrBuilder;

public class Engine
{
    Process proc;
    readonly StringBuilder stderr = new();

    public event Action<JsonElement> Event;      // on a reader thread
    public event Action<int, string> Exited;     // exit code, the tail of stderr

    public bool Running => proc != null && !proc.HasExited;

    public void Start(string python, string repo, IEnumerable<string> args)
    {
        var si = new ProcessStartInfo(python)
        {
            WorkingDirectory = repo, UseShellExecute = false, CreateNoWindow = true,
            RedirectStandardOutput = true, RedirectStandardError = true,
            StandardOutputEncoding = Encoding.UTF8, StandardErrorEncoding = Encoding.UTF8,
        };
        si.Environment["PYTHONUTF8"] = "1";
        si.Environment["PYTHONIOENCODING"] = "utf-8";
        foreach (var a in new[] { "-u", "-m", "builder" }.Concat(args)) si.ArgumentList.Add(a);
        stderr.Clear();
        var p = new Process { StartInfo = si, EnableRaisingEvents = true };
        proc = p;
        p.OutputDataReceived += (_, e) =>
        {
            if (string.IsNullOrWhiteSpace(e.Data)) return;
            try
            {
                using var doc = JsonDocument.Parse(e.Data);
                Event?.Invoke(doc.RootElement.Clone());
            }
            catch (JsonException) { }   // a stray line of a tool: the log has it
        };
        p.ErrorDataReceived += (_, e) => { if (e.Data != null) lock (stderr) stderr.AppendLine(e.Data); };
        p.Exited += (_, _) =>
        {
            p.WaitForExit();   // lets the readers drain
            string tail;
            lock (stderr) tail = stderr.Length > 1500 ? stderr.ToString(stderr.Length - 1500, 1500) : stderr.ToString();
            Exited?.Invoke(p.ExitCode, tail.Trim());
        };
        p.Start();
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
    }

    // Ctrl+Break to the builder's console, which also reaches the tools it runs (cli.py turns it into a clean stop:
    // the finished steps stay recorded). If the builder is still there ten seconds later, its process tree is ended.
    public void Cancel()
    {
        var p = proc;
        if (p == null || p.HasExited) return;
        bool sent = false;
        try
        {
            FreeConsole();
            if (AttachConsole((uint)p.Id))
            {
                // this process sits on that console for a moment and gets the event too: Ignore swallows it (a NULL
                // handler only covers Ctrl+C, and Ctrl+Break ended the window itself)
                SetConsoleCtrlHandler(Ignore, true);
                sent = GenerateConsoleCtrlEvent(1, 0);   // CTRL_BREAK_EVENT, everyone on that console
                Thread.Sleep(300);
                FreeConsole();
                SetConsoleCtrlHandler(Ignore, false);
            }
        }
        catch (Exception) { }
        var wait = sent ? 10000 : 0;
        Task.Run(() =>
        {
            try { if (!p.WaitForExit(wait)) p.Kill(true); } catch (Exception) { }
        });
    }

    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GenerateConsoleCtrlEvent(uint ev, uint group);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetConsoleCtrlHandler(CtrlHandler handler, bool add);
    delegate bool CtrlHandler(uint type);
    static readonly CtrlHandler Ignore = _ => true;   // kept alive here: the system calls it on a thread of its own
}
