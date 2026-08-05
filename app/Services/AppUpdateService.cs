using System.Net.Http;
using System.Net.Http.Headers;
using System.Reflection;
using System.Text.Json;

namespace xrFPS.ControlCenter.Services;

public sealed record UpdateInfo(
    string Version,
    string Name,
    string Notes,
    string InstallerName,
    string DownloadUrl,
    long SizeBytes,
    string? ChecksumsUrl,
    string HtmlUrl);

/// <summary>
/// Checks GitHub Releases for a newer Control Center and, on request, downloads
/// and launches the installer. Everything is best-effort: an offline machine,
/// a rate-limited API or a release without an installer asset simply means
/// "no update", never an error the user has to deal with.
/// </summary>
public sealed class AppUpdateService
{
    // The release list rather than /releases/latest: pre-releases count, since
    // beta users need to update between betas and /latest hides them.
    private const string ReleasesApiUrl = "https://api.github.com/repos/elliotttate/xrFPS/releases?per_page=15";
    private const string InstallerAssetSuffix = "-Setup.exe";
    private const string ChecksumsAssetName = "SHA256SUMS.txt";

    private static readonly HttpClient Http = CreateClient();

    private static HttpClient CreateClient()
    {
        var client = new HttpClient { Timeout = TimeSpan.FromSeconds(20) };
        // GitHub rejects requests without a user agent.
        client.DefaultRequestHeaders.UserAgent.Add(
            new ProductInfoHeaderValue("xrFPS-ControlCenter", CurrentVersion.ToString()));
        client.DefaultRequestHeaders.Accept.Add(
            new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        return client;
    }

    public static Version CurrentVersion =>
        Assembly.GetExecutingAssembly().GetName().Version ?? new Version(0, 0, 0);

    /// <summary>Returns the newer release, or null when already current.</summary>
    public async Task<UpdateInfo?> CheckAsync(CancellationToken token = default)
    {
        try
        {
            using var response = await Http.GetAsync(ReleasesApiUrl, token);
            if (!response.IsSuccessStatusCode)
            {
                return null;
            }

            await using var stream = await response.Content.ReadAsStreamAsync(token);
            using var document = await JsonDocument.ParseAsync(stream, cancellationToken: token);

            UpdateInfo? best = null;
            Version? bestVersion = null;

            foreach (var release in document.RootElement.EnumerateArray())
            {
                if (release.TryGetProperty("draft", out var draft) && draft.GetBoolean())
                {
                    continue;
                }

                string tag = release.TryGetProperty("tag_name", out var t) ? t.GetString() ?? "" : "";
                if (!TryParseVersion(tag, out Version? released) || released is null)
                {
                    continue;
                }

                // A release with no installer asset cannot be applied, so it is
                // not an update as far as this check is concerned.
                string installerName = "";
                string url = "";
                long size = 0;
                string? checksums = null;
                if (release.TryGetProperty("assets", out var assets))
                {
                    foreach (var asset in assets.EnumerateArray())
                    {
                        string name = asset.GetProperty("name").GetString() ?? "";
                        if (name.EndsWith(InstallerAssetSuffix, StringComparison.OrdinalIgnoreCase) &&
                            url.Length == 0)
                        {
                            installerName = name;
                            url = asset.GetProperty("browser_download_url").GetString() ?? "";
                            size = asset.TryGetProperty("size", out var s) ? s.GetInt64() : 0;
                        }
                        else if (name.Equals(ChecksumsAssetName, StringComparison.OrdinalIgnoreCase))
                        {
                            checksums = asset.GetProperty("browser_download_url").GetString();
                        }
                    }
                }
                if (url.Length == 0)
                {
                    continue;
                }

                if (bestVersion is null || released > bestVersion)
                {
                    bestVersion = released;
                    best = new UpdateInfo(
                        released.ToString(),
                        release.TryGetProperty("name", out var n) ? n.GetString() ?? tag : tag,
                        release.TryGetProperty("body", out var b) ? b.GetString() ?? "" : "",
                        installerName,
                        url,
                        size,
                        checksums,
                        release.TryGetProperty("html_url", out var h) ? h.GetString() ?? "" : "");
                }
            }

            return bestVersion is not null && bestVersion > CurrentVersion ? best : null;
        }
        catch (Exception)
        {
            // Offline, DNS failure, rate limit, malformed payload: treat all as
            // "nothing to update to" rather than bothering the user.
            return null;
        }
    }

    /// <summary>
    /// Downloads the installer to the temp directory and starts it. Returns the
    /// path so the caller can close the app, since the installer replaces files
    /// this process has loaded.
    /// </summary>
    public async Task<string?> DownloadAsync(UpdateInfo update, IProgress<double>? progress = null,
                                             CancellationToken token = default)
    {
        if (string.IsNullOrEmpty(update.DownloadUrl))
        {
            return null;
        }

        string dir = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "xrFPS", "updates");
        Directory.CreateDirectory(dir);
        string target = Path.Combine(dir, update.InstallerName);
        try
        {
            using var response = await Http.GetAsync(update.DownloadUrl,
                                                     HttpCompletionOption.ResponseHeadersRead, token);
            response.EnsureSuccessStatusCode();

            long total = response.Content.Headers.ContentLength ?? update.SizeBytes;
            await using var source = await response.Content.ReadAsStreamAsync(token);
            await using var file = File.Create(target);

            var buffer = new byte[81920];
            long copied = 0;
            int read;
            while ((read = await source.ReadAsync(buffer, token)) > 0)
            {
                await file.WriteAsync(buffer.AsMemory(0, read), token);
                copied += read;
                if (total > 0)
                {
                    progress?.Report((double)copied / total);
                }
            }
        }
        catch (Exception)
        {
            try
            {
                File.Delete(target);
            }
            catch
            {
                // Nothing useful to do if the partial file cannot be removed.
            }
            return null;
        }

        await VerifyChecksumAsync(update, target, token);
        return target;
    }

    /// <summary>
    /// Refuses an installer whose hash does not match the release manifest.
    /// A release that publishes no manifest is accepted on HTTPS alone.
    /// </summary>
    private static async Task VerifyChecksumAsync(UpdateInfo update, string installerPath,
                                                  CancellationToken token)
    {
        if (string.IsNullOrEmpty(update.ChecksumsUrl))
        {
            return;
        }

        string expected = "";
        try
        {
            string manifest = await Http.GetStringAsync(update.ChecksumsUrl, token);
            // Build-Release.ps1 writes "<hash> *<fileName>" per line.
            foreach (string line in manifest.Split('\n'))
            {
                string trimmed = line.Trim();
                if (trimmed.EndsWith(update.InstallerName, StringComparison.OrdinalIgnoreCase))
                {
                    expected = trimmed.Split(' ')[0].Trim();
                    break;
                }
            }
        }
        catch (Exception)
        {
            return; // Could not fetch the manifest; HTTPS remains the guarantee.
        }

        if (expected.Length == 0)
        {
            return;
        }

        await using var stream = File.OpenRead(installerPath);
        byte[] hash = await System.Security.Cryptography.SHA256.HashDataAsync(stream, token);
        string actual = Convert.ToHexString(hash);
        if (!actual.Equals(expected, StringComparison.OrdinalIgnoreCase))
        {
            stream.Close();
            File.Delete(installerPath);
            throw new InvalidOperationException(
                "The downloaded installer did not match the release checksum and was deleted.");
        }
    }

    /// <summary>
    /// Starts the installer silently and arranges for the app to come back.
    /// The caller must exit promptly afterwards so setup can replace files this
    /// process has loaded.
    /// </summary>
    public static bool StartUpdateAndRelaunch(string installerPath)
    {
        try
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(installerPath)
            {
                UseShellExecute = true,
                Arguments = "/VERYSILENT /SUPPRESSMSGBOXES /NORESTART",
            });
        }
        catch (Exception)
        {
            return false; // A declined UAC prompt lands here.
        }

        // The relaunch is done by a detached watcher rather than by the
        // installer: the installer that performs any given update was published
        // before this code existed, so relying on its own restart step would
        // only ever fix the update after next. The watcher also outlives this
        // process, which setup requires in order to replace its files.
        TryStartRelaunchWatcher(installerPath);
        return true;
    }

    private static void TryStartRelaunchWatcher(string installerPath)
    {
        try
        {
            string? appPath = Environment.ProcessPath;
            if (appPath is null)
            {
                return;
            }
            string appName = Path.GetFileNameWithoutExtension(appPath);
            // Setup runs as an elevated temporary copy of itself, and process
            // ids get recycled, so it is tracked by name prefix.
            string setupPrefix = Path.GetFileNameWithoutExtension(installerPath);
            string scriptPath = Path.Combine(Path.GetDirectoryName(installerPath)!,
                                             "relaunch-after-update.ps1");

            string script = $@"
$ErrorActionPreference = 'SilentlyContinue'
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline -and -not (Get-Process -Name '{setupPrefix}*')) {{ Start-Sleep -Milliseconds 500 }}
$deadline = (Get-Date).AddMinutes(10)
while ((Get-Date) -lt $deadline -and (Get-Process -Name '{setupPrefix}*')) {{ Start-Sleep -Seconds 1 }}
Start-Sleep -Seconds 2
if (-not (Get-Process -Name '{appName}')) {{ Start-Process '{appPath.Replace("'", "''")}' }}
";
            File.WriteAllText(scriptPath, script);

            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("powershell.exe")
            {
                UseShellExecute = false,
                CreateNoWindow = true,
                Arguments = $"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"{scriptPath}\"",
            });
        }
        catch (Exception)
        {
            // A missing watcher only costs a manual restart; it must never
            // abort an update that is already running.
        }
    }

    private static bool TryParseVersion(string tag, out Version? version)
    {
        version = null;
        string text = tag.TrimStart('v', 'V');

        // Drop any prerelease/build suffix: 0.3.0-beta.2 compares as 0.3.0.
        int cut = text.IndexOfAny(new[] { '-', '+' });
        if (cut > 0)
        {
            text = text[..cut];
        }
        return Version.TryParse(text, out version);
    }
}
