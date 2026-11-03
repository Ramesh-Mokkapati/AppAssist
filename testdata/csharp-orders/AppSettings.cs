using System.Collections.Generic;
using System.IO;
using System.Text.Json;

namespace Orders
{
    /// <summary>appsettings.json flattened to "Section:Key" names, like Microsoft.Extensions.Configuration.</summary>
    public sealed class AppSettings
    {
        private readonly Dictionary<string, string> values = new Dictionary<string, string>();

        public string? this[string key] => values.TryGetValue(key, out string? value) ? value : null;

        public static AppSettings Load(string path)
        {
            AppSettings settings = new AppSettings();
            if (File.Exists(path))
            {
                using JsonDocument doc = JsonDocument.Parse(File.ReadAllText(path));
                settings.Add(doc.RootElement, string.Empty);
            }

            return settings;
        }

        private void Add(JsonElement element, string prefix)
        {
            if (element.ValueKind == JsonValueKind.Object)
            {
                foreach (JsonProperty property in element.EnumerateObject())
                {
                    Add(property.Value, prefix.Length == 0 ? property.Name : prefix + ":" + property.Name);
                }
            }
            else
            {
                values[prefix] = element.ToString();
            }
        }
    }
}
