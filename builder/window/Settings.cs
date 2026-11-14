// The paths and choices of the last run, in %LOCALAPPDATA%\SrBuilder\settings.json. Paths only: nothing of the keys
// file or the game is read or stored here.
using System.IO;
using System.Text.Json;

namespace SrBuilder;

public class Settings
{
    public string Game { get; set; } = "";
    public string Firmware { get; set; } = "";
    public string Keys { get; set; } = "";
    public string Dest { get; set; } = "";
    public bool Desktop { get; set; } = true;
    public bool StartMenu { get; set; } = true;
    public bool DeleteOriginal { get; set; }

    static string FilePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                           "SrBuilder", "settings.json");

    public static Settings Load()
    {
        try { return JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath)) ?? new Settings(); }
        catch (Exception) { return new Settings(); }
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(FilePath));
            File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
        }
        catch (Exception) { }
    }
}
