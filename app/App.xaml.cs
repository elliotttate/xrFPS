using Microsoft.UI.Xaml;

namespace xrFPS.ControlCenter;

public partial class App : Application
{
    private Window? _window;

    public App()
    {
        InitializeComponent();
        // Exceptions thrown inside XAML callbacks do not reach the try/catch
        // in Main; without this hook the process exits with no UI at all.
        UnhandledException += (_, e) => Program.ReportFatal(e.Exception);
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        try
        {
            _window = new MainWindow();
            _window.Activate();
        }
        catch (Exception ex)
        {
            // The first native call into xrfps_core.dll happens in the window
            // constructor, so a load failure lands exactly here.
            Program.ReportFatal(ex);
            Environment.Exit(1);
        }
    }
}
