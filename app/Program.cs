using System.Runtime.InteropServices;
using System.Text;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;

namespace xrFPS.ControlCenter;

/// <summary>
/// Replaces the generated entry point so the installer (and scripts) can drive
/// registration headlessly with --install / --uninstall / --status, without
/// spinning up the UI.
/// </summary>
public static class Program
{
    [DllImport("kernel32.dll")]
    private static extern bool AttachConsole(int processId);

    [DllImport("kernel32.dll")]
    private static extern bool AllocConsole();

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    private static extern int MessageBoxW(IntPtr hwnd, string text, string caption, uint type);

    private static int _fatalReported;

    [STAThread]
    public static int Main(string[] args)
    {
        foreach (string arg in args)
        {
            switch (arg.ToLowerInvariant())
            {
                case "--install":
                    return RunVerb(install: true);
                case "--uninstall":
                    return RunVerb(install: false);
                case "--status":
                    return ReportStatus();
            }
        }

        // A WinExe that dies during startup shows nothing at all — the click
        // just doesn't "work". Every failure path must end in ReportFatal so
        // the user gets a dialog and a log they can attach to a bug report.
        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            if (e.ExceptionObject is Exception ex)
            {
                ReportFatal(ex);
            }
        };

        try
        {
            // Normal UI launch.
            global::WinRT.ComWrappersSupport.InitializeComWrappers();
            Application.Start(delegate
            {
                var context = new DispatcherQueueSynchronizationContext(
                    DispatcherQueue.GetForCurrentThread());
                SynchronizationContext.SetSynchronizationContext(context);
                _ = new App();
            });
        }
        catch (Exception ex)
        {
            ReportFatal(ex);
            return 1;
        }
        return 0;
    }

    /// <summary>
    /// Last-resort reporting for a UI that never came up (or died). XAML may
    /// be unusable at this point, so only Win32 is safe to touch.
    /// </summary>
    internal static void ReportFatal(Exception ex)
    {
        // XAML swallows some exceptions and rethrows others across the COM
        // boundary, so more than one hook can observe the same failure.
        if (Interlocked.Exchange(ref _fatalReported, 1) != 0)
        {
            return;
        }

        string logPath = "";
        try
        {
            string dir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "xrFPS");
            Directory.CreateDirectory(dir);
            logPath = Path.Combine(dir, "startup-crash.log");
            File.AppendAllText(logPath,
                $"[{DateTime.Now:yyyy-MM-dd HH:mm:ss}] {Environment.ProcessPath}{Environment.NewLine}" +
                $"{ex}{Environment.NewLine}{Environment.NewLine}");
        }
        catch
        {
            // A read-only profile still gets the dialog below.
        }

        const uint MB_ICONERROR = 0x10, MB_TOPMOST = 0x40000;
        MessageBoxW(IntPtr.Zero,
            $"xrFPS Control Center could not start.\n\n{ex.GetType().Name}: {ex.Message}" +
            (logPath.Length > 0 ? $"\n\nDetails were written to:\n{logPath}" : ""),
            "xrFPS Control Center", MB_ICONERROR | MB_TOPMOST);
    }

    private static int RunVerb(bool install)
    {
        var message = new StringBuilder(600);
        bool ok = install
            ? Native.xrfps_install(message, message.Capacity) != 0
            : Native.xrfps_uninstall(message, message.Capacity) != 0;

        Write(ok
            ? $"xrFPS: layer {(install ? "installed" : "uninstalled")}"
            : $"xrFPS: {(install ? "install" : "uninstall")} failed - {message}");
        return ok ? 0 : 1;
    }

    private static int ReportStatus()
    {
        var manifest = new StringBuilder(600);
        int flags = Native.xrfps_status(manifest, manifest.Capacity);
        Write($"xrFPS: {((flags & 1) != 0 ? "installed" : "not installed")}" +
              $"{((flags & 2) != 0 ? "" : " (layer files missing)")}\n  manifest: {manifest}");
        return (flags & 1) != 0 ? 0 : 1;
    }

    // A WinExe has no console of its own; attach to the caller's when there is
    // one so scripted runs can see the result.
    private static void Write(string text)
    {
        if (!AttachConsole(-1))
        {
            return; // launched without a console (e.g. by the installer)
        }
        Console.WriteLine(text);
    }
}
