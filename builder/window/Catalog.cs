// The builder's English texts (builder/text/en.json): step names, phases and messages, with {param} placeholders.
using System.IO;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace SrBuilder;

public class Catalog
{
    readonly Dictionary<string, string> text = new();

    public Catalog(string repo)
    {
        try
        {
            var path = Path.Combine(repo ?? "", "builder", "text", "en.json");
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            foreach (var p in doc.RootElement.EnumerateObject()) text[p.Name] = p.Value.GetString() ?? "";
        }
        catch (Exception) { }   // without the catalog the ids themselves are shown
    }

    public string Get(string id, JsonElement? pars = null)
    {
        var s = text.TryGetValue(id, out var t) ? t : id;
        return Regex.Replace(s, @"\{(\w+)\}", m =>
        {
            var name = m.Groups[1].Value;
            if (pars is JsonElement e && e.ValueKind == JsonValueKind.Object && e.TryGetProperty(name, out var v))
                return v.ValueKind == JsonValueKind.String ? v.GetString() : v.ToString();
            return name == "game_exe" ? "sr.exe" : m.Value;
        });
    }

    public string Get(string id, params (string, string)[] pars)
    {
        var s = text.TryGetValue(id, out var t) ? t : id;
        foreach (var (k, v) in pars) s = s.Replace("{" + k + "}", v);
        return s.Replace("{game_exe}", "sr.exe");
    }
}
