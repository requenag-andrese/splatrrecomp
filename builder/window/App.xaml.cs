// SR Builder's window. Options, all optional:
//   --repo DIR            the sr-recomp repository (default: found upward from the exe)
//   --game FILE --firmware DIR --keys FILE --dest DIR   fill the setup; --go starts the build at once
//   --shots DIR           draw each screen with sample values into DIR as PNG files and exit (a look check, no build)
//   --live-shots DIR      during a build, a PNG of the window at each step and at the end
//   --stop-after SECONDS  press STOP that long after the build starts (a test of the clean stop)
//   --toolchains DIR      the tools folder the builder uses (default: the workspace's .toolchains)
//   --vendor DIR          the pinned clones folder (default: the workspace's vendor)
using System.Windows;

namespace SrBuilder;

public class Options
{
    public string Repo, Game, Firmware, Keys, Dest, Shots, LiveShots, Toolchains, Vendor;
    public bool Go;
    public double StopAfter;
}

public partial class App : Application
{
    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        // the interface is English: numbers with a decimal point on any Windows language
        System.Globalization.CultureInfo.DefaultThreadCurrentCulture = System.Globalization.CultureInfo.InvariantCulture;
        System.Globalization.CultureInfo.CurrentCulture = System.Globalization.CultureInfo.InvariantCulture;
        var o = new Options();
        var a = e.Args;
        for (int i = 0; i < a.Length; i++)
        {
            string next() => i + 1 < a.Length ? a[++i] : "";
            switch (a[i])
            {
                case "--repo": o.Repo = next(); break;
                case "--game": o.Game = next(); break;
                case "--firmware": o.Firmware = next(); break;
                case "--keys": o.Keys = next(); break;
                case "--dest": o.Dest = next(); break;
                case "--shots": o.Shots = next(); break;
                case "--live-shots": o.LiveShots = next(); break;
                case "--go": o.Go = true; break;
                case "--toolchains": o.Toolchains = next(); break;
                case "--vendor": o.Vendor = next(); break;
                case "--stop-after": double.TryParse(next(), System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out o.StopAfter); break;
            }
        }
        var w = new MainWindow(o);
        if (o.Shots != null)
        {
            w.ShowInTaskbar = false;
            w.WindowStartupLocation = WindowStartupLocation.Manual;
            w.Left = -20000;
            w.Top = -20000;
        }
        w.Show();
    }
}
