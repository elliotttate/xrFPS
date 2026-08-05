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

        // Normal UI launch.
        global::WinRT.ComWrappersSupport.InitializeComWrappers();
        Application.Start(delegate
        {
            var context = new DispatcherQueueSynchronizationContext(
                DispatcherQueue.GetForCurrentThread());
            SynchronizationContext.SetSynchronizationContext(context);
            _ = new App();
        });
        return 0;
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
