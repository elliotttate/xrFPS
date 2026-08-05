using System.Runtime.InteropServices;
using System.Text;
using Microsoft.UI;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI;
using Path = System.IO.Path;

namespace FlipFPS.ControlCenter;

public sealed partial class MainWindow : Window
{
    private NativeConfig _config;
    private bool _loading = true;
    private readonly DispatcherQueueTimer _saveTimer;
    private readonly DispatcherQueueTimer _pollTimer;
    private readonly WriteableBitmap _preview;
    private readonly byte[] _previewPixels;
    private readonly int _hudW;
    private readonly int _hudH;
    private Button[] _navButtons = Array.Empty<Button>();
    private StackPanel[] _pages = Array.Empty<StackPanel>();

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hwnd);

    private static readonly (string Eyebrow, string Title, string Subtitle)[] PageInfo =
    {
        ("OVERLAY", "Dashboard", "Flip a controller upside down to see your frame rate in any OpenXR game."),
        ("OVERLAY", "Gesture", "Choose which controller summons the HUD and how deliberate the flip must be."),
        ("OVERLAY", "Placement & look", "Where the panel sits on the controller, and how it is drawn."),
        ("OVERLAY", "Readouts", "Pick exactly which numbers appear on the panel."),
        ("SETUP", "Installation", "Register FlipFPS with the OpenXR loader for your user account."),
    };

    public MainWindow()
    {
        InitializeComponent();
        Title = "FlipFPS Control Center";
        ExtendsContentIntoTitleBar = true;

        // AppWindow works in physical pixels while the layout is in DIPs, so
        // the requested size has to be scaled or the content is cramped on a
        // high-DPI display.
        IntPtr hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        double scale = GetDpiForWindow(hwnd) / 96.0;
        AppWindow.Resize(new Windows.Graphics.SizeInt32((int)(1320 * scale), (int)(880 * scale)));

        _hudW = Native.flipfps_hud_width();
        _hudH = Native.flipfps_hud_height();
        _previewPixels = new byte[_hudW * _hudH * 4];
        _preview = new WriteableBitmap(_hudW, _hudH);
        PreviewImage.Source = _preview;
        PreviewImage2.Source = _preview;
        PreviewImage3.Source = _preview;

        _navButtons = new[] { NavDashboard, NavGesture, NavAppearance, NavReadouts, NavInstall };
        _pages = new[] { PageDashboard, PageGesture, PageAppearance, PageReadouts, PageInstall };

        Native.flipfps_config_load(out _config);
        ApplyConfigToControls();
        Native.flipfps_metrics_start();

        var queue = DispatcherQueue.GetForCurrentThread();

        // Coalesce slider drags into one write; the layer polls this file.
        _saveTimer = queue.CreateTimer();
        _saveTimer.Interval = TimeSpan.FromMilliseconds(400);
        _saveTimer.IsRepeating = false;
        _saveTimer.Tick += (_, _) => Native.flipfps_config_save(ref _config);

        _pollTimer = queue.CreateTimer();
        _pollTimer.Interval = TimeSpan.FromMilliseconds(500);
        _pollTimer.IsRepeating = true;
        _pollTimer.Tick += (_, _) => Poll();
        _pollTimer.Start();

        Closed += (_, _) =>
        {
            Native.flipfps_config_save(ref _config);
            Native.flipfps_metrics_stop();
        };

        SelectPage(0);
        RefreshStatus();
        Poll();
    }

    // ---- navigation -------------------------------------------------

    private void Nav_Click(object sender, RoutedEventArgs e)
    {
        if (sender is Button b && int.TryParse(b.Tag?.ToString(), out int index))
        {
            SelectPage(index);
        }
    }

    private void SelectPage(int index)
    {
        for (int i = 0; i < _pages.Length; ++i)
        {
            _pages[i].Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
            _navButtons[i].Background = i == index
                ? new SolidColorBrush(Color.FromArgb(0x24, 0x56, 0xCF, 0x85))
                : new SolidColorBrush(Colors.Transparent);
            _navButtons[i].Foreground = i == index
                ? new SolidColorBrush(Colors.White)
                : (SolidColorBrush)Application.Current.Resources["MutedTextBrush"];
        }
        PageEyebrow.Text = PageInfo[index].Eyebrow;
        PageTitle.Text = PageInfo[index].Title;
        PageSubtitle.Text = PageInfo[index].Subtitle;
    }

    // ---- config <-> controls ---------------------------------------

    private void ApplyConfigToControls()
    {
        _loading = true;
        EnabledToggle.IsOn = _config.Enabled != 0;
        HandCombo.SelectedIndex = Math.Clamp(_config.Hand, 0, 2);
        FlipAngleSlider.Value = _config.FlipAngleDeg;
        ShowDelaySlider.Value = _config.ShowDelaySec * 100.0;
        HideDelaySlider.Value = _config.HideDelaySec * 100.0;
        F10Check.IsChecked = _config.F10Toggle != 0;

        PanelWidthSlider.Value = _config.PanelWidthM * 100.0;
        PanelOffsetSlider.Value = _config.PanelOffsetM * 100.0;
        PanelRollSlider.Value = _config.PanelRollDeg;
        OpacitySlider.Value = _config.Opacity * 100.0;
        ThemeCombo.SelectedIndex = Math.Clamp(_config.Theme, 0, 2);
        RefreshSlider.Value = _config.HudRefreshHz * 10.0;

        ShowFps.IsChecked = _config.ShowFps != 0;
        ShowFrametime.IsChecked = _config.ShowFrametime != 0;
        ShowTarget.IsChecked = _config.ShowTarget != 0;
        ShowOnTarget.IsChecked = _config.ShowOnTarget != 0;
        ShowPercentiles.IsChecked = _config.ShowPercentiles != 0;
        ShowGraph.IsChecked = _config.ShowGraph != 0;
        ShowCpuUsage.IsChecked = _config.ShowCpuUsage != 0;
        ShowGpuUsage.IsChecked = _config.ShowGpuUsage != 0;
        ShowGpuAppUsage.IsChecked = _config.ShowGpuAppUsage != 0;
        ShowVram.IsChecked = _config.ShowVram != 0;
        ShowGpuTemp.IsChecked = _config.ShowGpuTemp != 0;
        ShowCpuTemp.IsChecked = _config.ShowCpuTemp != 0;
        ShowGpuClock.IsChecked = _config.ShowGpuClock != 0;
        ShowRam.IsChecked = _config.ShowRam != 0;
        _loading = false;
        UpdateValueLabels();
    }

    private void Setting_Changed(object sender, object e)
    {
        if (_loading)
        {
            return;
        }

        _config.Enabled = EnabledToggle.IsOn ? 1 : 0;
        _config.Hand = HandCombo.SelectedIndex < 0 ? 0 : HandCombo.SelectedIndex;
        _config.FlipAngleDeg = (float)FlipAngleSlider.Value;
        _config.ShowDelaySec = (float)(ShowDelaySlider.Value / 100.0);
        _config.HideDelaySec = (float)(HideDelaySlider.Value / 100.0);
        _config.F10Toggle = F10Check.IsChecked == true ? 1 : 0;

        _config.PanelWidthM = (float)(PanelWidthSlider.Value / 100.0);
        _config.PanelOffsetM = (float)(PanelOffsetSlider.Value / 100.0);
        _config.PanelRollDeg = (float)PanelRollSlider.Value;
        _config.Opacity = (float)(OpacitySlider.Value / 100.0);
        _config.Theme = ThemeCombo.SelectedIndex < 0 ? 0 : ThemeCombo.SelectedIndex;
        _config.HudRefreshHz = (float)(RefreshSlider.Value / 10.0);

        _config.ShowFps = ShowFps.IsChecked == true ? 1 : 0;
        _config.ShowFrametime = ShowFrametime.IsChecked == true ? 1 : 0;
        _config.ShowTarget = ShowTarget.IsChecked == true ? 1 : 0;
        _config.ShowOnTarget = ShowOnTarget.IsChecked == true ? 1 : 0;
        _config.ShowPercentiles = ShowPercentiles.IsChecked == true ? 1 : 0;
        _config.ShowGraph = ShowGraph.IsChecked == true ? 1 : 0;
        _config.ShowCpuUsage = ShowCpuUsage.IsChecked == true ? 1 : 0;
        _config.ShowGpuUsage = ShowGpuUsage.IsChecked == true ? 1 : 0;
        _config.ShowGpuAppUsage = ShowGpuAppUsage.IsChecked == true ? 1 : 0;
        _config.ShowVram = ShowVram.IsChecked == true ? 1 : 0;
        _config.ShowGpuTemp = ShowGpuTemp.IsChecked == true ? 1 : 0;
        _config.ShowCpuTemp = ShowCpuTemp.IsChecked == true ? 1 : 0;
        _config.ShowGpuClock = ShowGpuClock.IsChecked == true ? 1 : 0;
        _config.ShowRam = ShowRam.IsChecked == true ? 1 : 0;

        UpdateValueLabels();
        RenderPreview();
        _saveTimer.Start();
    }

    private void UpdateValueLabels()
    {
        FlipAngleLabel.Text = $"Flip angle — {_config.FlipAngleDeg:0} degrees past upright";
        ShowDelayLabel.Text = $"Show delay — {_config.ShowDelaySec:0.00} s";
        HideDelayLabel.Text = $"Hide delay — {_config.HideDelaySec:0.00} s";
        PanelWidthLabel.Text = $"Panel width — {_config.PanelWidthM:0.00} m";
        PanelOffsetLabel.Text = $"Distance from controller — {_config.PanelOffsetM:0.00} m";
        PanelRollLabel.Text = $"Roll — {_config.PanelRollDeg:0} degrees";
        OpacityLabel.Text = $"Opacity — {_config.Opacity * 100:0} %";
        RefreshLabel.Text = $"HUD refresh — {_config.HudRefreshHz:0.0} Hz";
    }

    // ---- live loop --------------------------------------------------

    private void Poll()
    {
        Native.flipfps_metrics_sample();
        Native.flipfps_metrics_get(out NativeMetrics m);

        TileCpu.Text = m.CpuUsagePct >= 0 ? $"{m.CpuUsagePct:0}%" : "--";
        TileGpu.Text = m.GpuUsagePct >= 0 ? $"{m.GpuUsagePct:0}%" : "--";
        TileVram.Text = m.VramUsedMb >= 0 ? $"{m.VramUsedMb / 1024f:0.0} GB" : "--";
        TileTemp.Text = m.GpuTempC >= 0 ? $"{m.GpuTempC:0}°C" : "--";
        TileApp.Text = m.AppPid != 0 ? m.AppName : "none detected";

        var status = new StringBuilder(128);
        Native.flipfps_metrics_status(status, status.Capacity);
        CollectorStatus.Text = status.ToString();

        RenderPreview();
    }

    private void RenderPreview()
    {
        Native.flipfps_hud_render(ref _config, 1, 11.1f, _previewPixels);
        // WriteableBitmap's buffer is BGRA8, which is what the native
        // renderer produced, so this is a straight copy.
        unsafe
        {
            var access = WinRT.CastExtensions.As<IBufferByteAccess>(_preview.PixelBuffer);
            access.Buffer(out byte* target);
            Marshal.Copy(_previewPixels, 0, (IntPtr)target, _previewPixels.Length);
        }
        _preview.Invalidate();
    }

    // ---- install ----------------------------------------------------

    private void RefreshStatus()
    {
        var manifest = new StringBuilder(600);
        int flags = Native.flipfps_status(manifest, manifest.Capacity);
        bool registered = (flags & 1) != 0;
        bool filesPresent = (flags & 2) != 0;

        var good = (SolidColorBrush)Application.Current.Resources["GoodBrush"];
        var warn = (SolidColorBrush)Application.Current.Resources["WarnBrush"];
        var bad = (SolidColorBrush)Application.Current.Resources["BadBrush"];

        if (registered && filesPresent)
        {
            StatusDot.Fill = good;
            StatusHeadline.Text = "Layer installed";
            StatusDetail.Text = "Every OpenXR app you start from now on loads FlipFPS automatically.";
            SidebarStatusText.Text = "Installed";
            SidebarStatusText.Foreground = good;
            SidebarHintText.Text = "Active for this user";
        }
        else if (registered)
        {
            StatusDot.Fill = warn;
            StatusHeadline.Text = "Registered, files missing";
            StatusDetail.Text = "The registry points at a layer that is not on disk. Use Update files to repair it.";
            SidebarStatusText.Text = "Needs repair";
            SidebarStatusText.Foreground = warn;
            SidebarHintText.Text = "Files missing";
        }
        else
        {
            StatusDot.Fill = bad;
            StatusHeadline.Text = "Not installed";
            StatusDetail.Text = "Install the layer to have OpenXR games load FlipFPS. No administrator rights needed.";
            SidebarStatusText.Text = "Not installed";
            SidebarStatusText.Foreground = bad;
            SidebarHintText.Text = "Press Install layer";
        }

        InstallButton.Visibility = registered ? Visibility.Collapsed : Visibility.Visible;
        UninstallButton.Visibility = registered ? Visibility.Visible : Visibility.Collapsed;
        ReinstallButton.Visibility = registered ? Visibility.Visible : Visibility.Collapsed;

        ManifestPathText.Text = manifest.ToString();
        var configPath = new StringBuilder(600);
        Native.flipfps_config_path(configPath, configPath.Capacity);
        ConfigPathText.Text = configPath.ToString();
    }

    private void Install_Click(object sender, RoutedEventArgs e)
    {
        var error = new StringBuilder(600);
        bool ok = Native.flipfps_install(error, error.Capacity) != 0;
        ToastText.Text = ok
            ? "Layer installed. Launch or relaunch an OpenXR game to load it."
            : $"Install failed: {error}";
        RefreshStatus();
    }

    private void Uninstall_Click(object sender, RoutedEventArgs e)
    {
        var error = new StringBuilder(600);
        bool ok = Native.flipfps_uninstall(error, error.Capacity) != 0;
        ToastText.Text = ok ? "Layer uninstalled." : $"Uninstall failed: {error}";
        RefreshStatus();
    }

    private void RestoreDefaults_Click(object sender, RoutedEventArgs e)
    {
        Native.flipfps_config_defaults(out _config);
        ApplyConfigToControls();
        Native.flipfps_config_save(ref _config);
        RenderPreview();
        ToastText.Text = "Settings restored to defaults.";
    }

    private void OpenFolder_Click(object sender, RoutedEventArgs e)
    {
        string dir = Path.GetDirectoryName(ManifestPathText.Text) ?? "";
        if (Directory.Exists(dir))
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(dir) { UseShellExecute = true });
        }
    }

    private void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        string dir = Path.GetDirectoryName(ManifestPathText.Text) ?? "";
        string log = Path.Combine(dir, "flipfps.log");
        if (File.Exists(log))
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(log) { UseShellExecute = true });
        }
        else
        {
            ToastText.Text = "No log yet — it appears once a game has loaded the layer.";
        }
    }
}
