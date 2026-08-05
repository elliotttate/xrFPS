using System.Runtime.InteropServices;
using System.Text;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Microsoft.UI.Xaml.Media.Imaging;
using Windows.UI;
using xrFPS.ControlCenter.Services;
using Path = System.IO.Path;

namespace xrFPS.ControlCenter;

public sealed partial class MainWindow : Window
{
    private NativeConfig _config;
    private bool _loading = true;
    private bool _syncingPreviewRate;
    private bool _installBusy;
    private readonly DispatcherQueueTimer _saveTimer;
    private readonly CancellationTokenSource _shutdown = new();
    private readonly Services.AppUpdateService _updates = new();
    private readonly object _metricsGate = new();
    private Task? _sampler;
    private readonly WriteableBitmap _preview;
    private readonly byte[] _previewPixels;
    private readonly int _hudW;
    private readonly int _hudH;
    private float _previewTargetMs = 1000.0f / 90.0f;
    private NavigationViewItem[] _navItems = Array.Empty<NavigationViewItem>();
    private StackPanel[] _pages = Array.Empty<StackPanel>();
    private ComboBox[] _previewRateCombos = Array.Empty<ComboBox>();

    private static readonly int[] PreviewRates = { 72, 80, 90, 120, 144 };

    [DllImport("user32.dll")]
    private static extern uint GetDpiForWindow(IntPtr hwnd);

    private static readonly (string Eyebrow, string Title, string Subtitle)[] PageInfo =
    {
        ("OVERLAY", "Dashboard", "Flip a controller upside down to see your frame rate in any OpenXR game."),
        ("OVERLAY", "Gesture", "Choose which controller summons the HUD and how deliberate the flip must be."),
        ("OVERLAY", "Placement & look", "Control where the HUD sits and how it appears in the headset."),
        ("OVERLAY", "Readouts", "Choose the frame statistics and system metrics shown on the HUD."),
        ("SETUP", "Installation", "Install, repair, or remove the per-user OpenXR API layer."),
    };

    public MainWindow()
    {
        InitializeComponent();
        Title = "xrFPS Control Center";
        ExtendsContentIntoTitleBar = true;

        // AppWindow sizes are physical pixels while XAML uses DIPs.
        IntPtr hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        double scale = Math.Max(1.0, GetDpiForWindow(hwnd) / 96.0);
        AppWindow.Resize(new Windows.Graphics.SizeInt32((int)(1320 * scale), (int)(880 * scale)));
        try
        {
            string icon = Path.Combine(AppContext.BaseDirectory, "Assets", "xrFPS.ico");
            if (File.Exists(icon))
            {
                AppWindow.SetIcon(icon);
            }
        }
        catch
        {
            // An embedded executable icon still provides taskbar branding.
        }

        _hudW = Native.xrfps_hud_width();
        _hudH = Native.xrfps_hud_height();
        _previewPixels = new byte[_hudW * _hudH * 4];
        _preview = new WriteableBitmap(_hudW, _hudH);
        PreviewImage.Source = _preview;
        PreviewImage2.Source = _preview;
        PreviewImage3.Source = _preview;

        _navItems = new[] { NavDashboard, NavGesture, NavAppearance, NavReadouts, NavInstall };
        _pages = new[] { PageDashboard, PageGesture, PageAppearance, PageReadouts, PageInstall };
        _previewRateCombos = new[] { PreviewRateCombo1, PreviewRateCombo2, PreviewRateCombo3 };
        _syncingPreviewRate = true;
        foreach (ComboBox combo in _previewRateCombos)
        {
            combo.SelectedIndex = 2;
        }
        _syncingPreviewRate = false;

        Native.xrfps_config_load(out _config);
        ApplyConfigToControls();
        lock (_metricsGate)
        {
            Native.xrfps_metrics_start();
        }

        DispatcherQueue queue = DispatcherQueue.GetForCurrentThread();

        // Coalesce slider drags into one atomic config write.
        _saveTimer = queue.CreateTimer();
        _saveTimer.Interval = TimeSpan.FromMilliseconds(400);
        _saveTimer.IsRepeating = false;
        _saveTimer.Tick += (_, _) =>
        {
            if (Native.xrfps_config_save(ref _config) == 0)
            {
                ShowAction("Settings were not saved", "Check that the xrFPS settings folder is writable.",
                           InfoBarSeverity.Error);
            }
        };

        // PDH/NVML calls can block, so all sampling stays off the UI thread.
        _sampler = Task.Run(async () =>
        {
            while (!_shutdown.IsCancellationRequested)
            {
                NativeMetrics metrics;
                string status;
                lock (_metricsGate)
                {
                    if (_shutdown.IsCancellationRequested)
                    {
                        return;
                    }
                    Native.xrfps_metrics_sample();
                    Native.xrfps_metrics_get(out metrics);
                    var text = new StringBuilder(128);
                    Native.xrfps_metrics_status(text, text.Capacity);
                    status = text.ToString();
                }
                queue.TryEnqueue(() => ShowMetrics(metrics, status));

                try
                {
                    await Task.Delay(500, _shutdown.Token);
                }
                catch (TaskCanceledException)
                {
                    return;
                }
            }
        });

        Closed += (_, _) =>
        {
            _shutdown.Cancel();
            Native.xrfps_config_save(ref _config);
            lock (_metricsGate)
            {
                Native.xrfps_metrics_stop();
            }
        };

        MainNavigation.SelectedItem = NavDashboard;
        SelectPage(0);
        RefreshStatus();
        RenderPreview();

        VersionText.Text = $"You are running xrFPS {Services.AppUpdateService.CurrentVersion}.";

        // One quiet check per launch, after the window is up.
        Activated += MainWindow_FirstActivated;
    }

    private async void MainWindow_FirstActivated(object sender, WindowActivatedEventArgs args)
    {
        Activated -= MainWindow_FirstActivated;
        await CheckForUpdatesAsync(userInitiated: false);
    }

    // ---- navigation and responsive layout ---------------------------

    private void Navigation_SelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItemContainer is NavigationViewItem item &&
            int.TryParse(item.Tag?.ToString(), out int index))
        {
            SelectPage(index);
        }
    }

    private void SelectPage(int index)
    {
        if (index < 0 || index >= _pages.Length)
        {
            return;
        }
        for (int i = 0; i < _pages.Length; ++i)
        {
            _pages[i].Visibility = i == index ? Visibility.Visible : Visibility.Collapsed;
        }
        PageEyebrow.Text = PageInfo[index].Eyebrow;
        PageTitle.Text = PageInfo[index].Title;
        PageSubtitle.Text = PageInfo[index].Subtitle;
    }

    private void RootGrid_SizeChanged(object sender, SizeChangedEventArgs e)
    {
        bool useSidePreview = e.NewSize.Width >= 1040;
        SetPreviewLayout(DashboardTopGrid, DashboardPreviewCard, useSidePreview);
        SetPreviewLayout(AppearanceGrid, AppearancePreviewCard, useSidePreview);
        SetPreviewLayout(ReadoutsGrid, ReadoutsPreviewCard, useSidePreview);

        int metricColumns = e.NewSize.Width >= 940 ? 5 : e.NewSize.Width >= 620 ? 3 : 2;
        FrameworkElement[] tiles = { MetricCpu, MetricGpu, MetricVram, MetricTemp, MetricApp };
        for (int i = 0; i < MetricTiles.ColumnDefinitions.Count; ++i)
        {
            MetricTiles.ColumnDefinitions[i].Width = i < metricColumns
                ? new GridLength(1, GridUnitType.Star)
                : new GridLength(0);
        }
        for (int i = 0; i < tiles.Length; ++i)
        {
            Grid.SetColumn(tiles[i], i % metricColumns);
            Grid.SetRow(tiles[i], i / metricColumns);
        }
    }

    private static void SetPreviewLayout(Grid grid, FrameworkElement preview, bool sideBySide)
    {
        grid.ColumnDefinitions[1].Width = sideBySide ? new GridLength(330) : new GridLength(0);
        Grid.SetColumn(preview, sideBySide ? 1 : 0);
        Grid.SetRow(preview, sideBySide ? 0 : 1);
        preview.MaxWidth = sideBySide ? 330 : double.PositiveInfinity;
        preview.HorizontalAlignment = HorizontalAlignment.Stretch;
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
        TimingSourceCombo.SelectedIndex = Math.Clamp(_config.TimingSource, 0, 3);

        PanelWidthSlider.Value = _config.PanelWidthM * 100.0;
        PanelOffsetSlider.Value = _config.PanelOffsetM * 100.0;
        PanelRollSlider.Value = _config.PanelRollDeg;
        OpacitySlider.Value = _config.Opacity * 100.0;
        ThemeCombo.SelectedIndex = Math.Clamp(_config.Theme, 0, 5);
        RefreshSlider.Value = _config.HudRefreshHz * 10.0;

        ShowFps.IsChecked = _config.ShowFps != 0;
        ShowFrametime.IsChecked = _config.ShowFrametime != 0;
        ShowCpuFrametime.IsChecked = _config.ShowCpuFrametime != 0;
        ShowGpuFrametime.IsChecked = _config.ShowGpuFrametime != 0;
        ShowTarget.IsChecked = _config.ShowTarget != 0;
        ShowOnTarget.IsChecked = _config.ShowOnTarget != 0;
        ShowPercentiles.IsChecked = _config.ShowPercentiles != 0;
        ShowGraph.IsChecked = _config.ShowGraph != 0;
        GraphModeCombo.SelectedIndex = Math.Clamp(_config.GraphMode, 0, 3);
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
        _config.TimingSource = TimingSourceCombo.SelectedIndex < 0 ? 0 : TimingSourceCombo.SelectedIndex;

        _config.PanelWidthM = (float)(PanelWidthSlider.Value / 100.0);
        _config.PanelOffsetM = (float)(PanelOffsetSlider.Value / 100.0);
        _config.PanelRollDeg = (float)PanelRollSlider.Value;
        _config.Opacity = (float)(OpacitySlider.Value / 100.0);
        _config.Theme = ThemeCombo.SelectedIndex < 0 ? 0 : ThemeCombo.SelectedIndex;
        _config.HudRefreshHz = (float)(RefreshSlider.Value / 10.0);

        _config.ShowFps = ShowFps.IsChecked == true ? 1 : 0;
        _config.ShowFrametime = ShowFrametime.IsChecked == true ? 1 : 0;
        _config.ShowCpuFrametime = ShowCpuFrametime.IsChecked == true ? 1 : 0;
        _config.ShowGpuFrametime = ShowGpuFrametime.IsChecked == true ? 1 : 0;
        _config.ShowTarget = ShowTarget.IsChecked == true ? 1 : 0;
        _config.ShowOnTarget = ShowOnTarget.IsChecked == true ? 1 : 0;
        _config.ShowPercentiles = ShowPercentiles.IsChecked == true ? 1 : 0;
        _config.ShowGraph = ShowGraph.IsChecked == true ? 1 : 0;
        _config.GraphMode = GraphModeCombo.SelectedIndex < 0 ? 0 : GraphModeCombo.SelectedIndex;
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
        FlipAngleLabel.Text = $"Flip angle — {_config.FlipAngleDeg:0}° from upright";
        ShowDelayLabel.Text = $"Show delay — {_config.ShowDelaySec:0.00} s";
        HideDelayLabel.Text = $"Hide delay — {_config.HideDelaySec:0.00} s";
        PanelWidthLabel.Text = $"Panel width — {_config.PanelWidthM * 100:0} cm";
        PanelOffsetLabel.Text = $"Distance from controller — {_config.PanelOffsetM * 100:0} cm";
        PanelRollLabel.Text = $"Roll — {_config.PanelRollDeg:0}°";
        OpacityLabel.Text = $"Opacity — {_config.Opacity * 100:0}%";
        RefreshLabel.Text = $"HUD refresh — {_config.HudRefreshHz:0.0} Hz";
        ShowConfiguredTimingStatus();
    }

    private void ShowConfiguredTimingStatus()
    {
        TimingSourceStatus.Text = _config.TimingSource switch
        {
            1 => "SteamVR compositor timing is forced. OpenXR submission timing is used if SteamVR is unavailable.",
            2 => "Meta runtime counters are forced. OpenXR submission timing is used if XR_META_performance_metrics is unavailable.",
            3 => "Portable OpenXR mode measures successful frame submissions and the CPU submission span.",
            _ => "Auto uses SteamVR compositor timing on SteamVR, Meta metrics on Meta runtimes, then falls back to successful OpenXR submissions."
        };
    }

    // ---- live metrics and preview ----------------------------------

    private void ShowMetrics(NativeMetrics metrics, string status)
    {
        if (_shutdown.IsCancellationRequested)
        {
            return;
        }

        TileCpu.Text = metrics.CpuUsagePct >= 0 ? $"{metrics.CpuUsagePct:0}%" : "--";
        TileGpu.Text = metrics.GpuUsagePct >= 0 ? $"{metrics.GpuUsagePct:0}%" : "--";
        TileVram.Text = metrics.VramUsedMb >= 0 ? $"{metrics.VramUsedMb / 1024f:0.0} GB" : "--";
        TileTemp.Text = metrics.GpuTempC >= 0 ? $"{metrics.GpuTempC:0}°C" : "--";
        TileApp.Text = metrics.AppPid != 0 ? metrics.AppName : "none detected";
        CollectorStatus.Text = status;

        if (metrics.AppPid != 0 && !string.IsNullOrWhiteSpace(metrics.TimingSourceName))
        {
            string values = metrics.AppFps > 0
                ? $" · {metrics.AppFps:0.0} FPS · {metrics.AppFrameMs:0.00} ms"
                : string.Empty;
            string split = metrics.AppCpuFrameMs > 0 || metrics.AppGpuFrameMs > 0
                ? $" · CPU {(metrics.AppCpuFrameMs > 0 ? $"{metrics.AppCpuFrameMs:0.00} ms" : "—")} · GPU {(metrics.AppGpuFrameMs > 0 ? $"{metrics.AppGpuFrameMs:0.00} ms" : "—")}"
                : string.Empty;
            TimingSourceStatus.Text = $"Active: {metrics.TimingSourceName}{values}{split}";
        }
        else
        {
            ShowConfiguredTimingStatus();
        }

        bool hasNvml = status.Contains("NVML", StringComparison.OrdinalIgnoreCase);
        SetMetricAvailability(ShowVram, hasNvml, "VRAM", "VRAM (unavailable)");
        SetMetricAvailability(ShowGpuClock, hasNvml, "GPU clock", "GPU clock (unavailable)");
        SetMetricAvailability(ShowGpuTemp, hasNvml, "GPU temperature", "GPU temperature (unavailable)");
        SetMetricAvailability(ShowCpuTemp, false, "CPU temperature", "CPU temperature (unavailable)");
        MetricCapabilitySummary.Text = hasNvml
            ? "PDH and NVIDIA NVML are available. Per-game GPU usage appears when an OpenXR app loads the layer; CPU temperature has no provider yet."
            : "PDH is available. VRAM, GPU temperature, and GPU clock need an NVIDIA NVML provider; CPU temperature has no provider yet.";
        RenderPreview();
    }

    private static void SetMetricAvailability(CheckBox box, bool available, string label, string unavailableLabel)
    {
        box.IsEnabled = available;
        box.Content = available ? label : unavailableLabel;
    }

    private void PreviewRate_Changed(object sender, SelectionChangedEventArgs e)
    {
        if (_syncingPreviewRate || sender is not ComboBox combo || combo.SelectedIndex < 0)
        {
            return;
        }
        int index = Math.Clamp(combo.SelectedIndex, 0, PreviewRates.Length - 1);
        _syncingPreviewRate = true;
        foreach (ComboBox other in _previewRateCombos)
        {
            other.SelectedIndex = index;
        }
        _syncingPreviewRate = false;
        _previewTargetMs = 1000.0f / PreviewRates[index];
        RenderPreview();
    }

    private void RenderPreview()
    {
        lock (_metricsGate)
        {
            Native.xrfps_hud_render(ref _config, 1, _previewTargetMs, _previewPixels);
        }
        unsafe
        {
            var access = WinRT.CastExtensions.As<IBufferByteAccess>(_preview.PixelBuffer);
            access.Buffer(out byte* target);
            Marshal.Copy(_previewPixels, 0, (IntPtr)target, _previewPixels.Length);
        }
        _preview.Invalidate();
    }

    // ---- install and reset actions ---------------------------------

    private void RefreshStatus()
    {
        var manifest = new StringBuilder(600);
        int flags = Native.xrfps_status(manifest, manifest.Capacity);
        bool registered = (flags & 1) != 0;
        bool filesPresent = (flags & 2) != 0;

        var good = (SolidColorBrush)Application.Current.Resources["GoodBrush"];
        var warn = (SolidColorBrush)Application.Current.Resources["WarnBrush"];
        var bad = (SolidColorBrush)Application.Current.Resources["BadBrush"];

        Brush stateBrush;
        string headline;
        string detail;
        if (registered && filesPresent)
        {
            stateBrush = good;
            headline = "Layer installed";
            detail = "Every OpenXR app started from now on loads xrFPS automatically.";
        }
        else if (registered)
        {
            stateBrush = warn;
            headline = "Registered, files missing";
            detail = "The registry points to files that are no longer on disk. Use Update files to repair the installation.";
        }
        else
        {
            stateBrush = bad;
            headline = "Not installed";
            detail = "Install the layer to make it available to OpenXR apps. Administrator rights are not required.";
        }

        StatusDot.Fill = stateBrush;
        InstallPageStatusDot.Fill = stateBrush;
        StatusHeadline.Text = headline;
        InstallPageStatusHeadline.Text = headline;
        StatusDetail.Text = detail;
        InstallPageStatusDetail.Text = detail;

        SetInstallButtonVisibility(registered);
        ManifestPathText.Text = manifest.ToString();
        var configPath = new StringBuilder(600);
        Native.xrfps_config_path(configPath, configPath.Capacity);
        ConfigPathText.Text = configPath.ToString();
    }

    private void SetInstallButtonVisibility(bool registered)
    {
        foreach (Button button in new[] { InstallButton, InstallButton2 })
        {
            button.Visibility = registered ? Visibility.Collapsed : Visibility.Visible;
        }
        foreach (Button button in new[] { UninstallButton, UninstallButton2, ReinstallButton, ReinstallButton2 })
        {
            button.Visibility = registered ? Visibility.Visible : Visibility.Collapsed;
        }
    }

    private void SetInstallBusy(bool busy)
    {
        _installBusy = busy;
        foreach (Button button in new[]
                 {
                     InstallButton, InstallButton2, UninstallButton, UninstallButton2,
                     ReinstallButton, ReinstallButton2,
                 })
        {
            button.IsEnabled = !busy;
        }
    }

    private async void Install_Click(object sender, RoutedEventArgs e)
    {
        if (_installBusy)
        {
            return;
        }
        bool updating = sender is Button button && button.Name.StartsWith("Reinstall", StringComparison.Ordinal);
        SetInstallBusy(true);
        ShowAction(updating ? "Updating layer files…" : "Installing layer…",
                   "xrFPS is updating the per-user OpenXR registration.", InfoBarSeverity.Informational);
        var error = new StringBuilder(600);
        bool ok = await Task.Run(() => Native.xrfps_install(error, error.Capacity) != 0);
        SetInstallBusy(false);
        RefreshStatus();
        ShowAction(ok ? (updating ? "Layer files updated" : "Layer installed") : "Installation failed",
                   ok ? "Launch or relaunch an OpenXR app to load this build." : error.ToString(),
                   ok ? InfoBarSeverity.Success : InfoBarSeverity.Error);
    }

    private async void Uninstall_Click(object sender, RoutedEventArgs e)
    {
        if (_installBusy)
        {
            return;
        }
        var dialog = new ContentDialog
        {
            XamlRoot = RootGrid.XamlRoot,
            Title = "Uninstall the xrFPS layer?",
            Content = "This removes the per-user OpenXR registration. Your settings remain on disk and you can reinstall at any time.",
            PrimaryButtonText = "Uninstall",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        SetInstallBusy(true);
        ShowAction("Uninstalling layer…", "Removing the per-user OpenXR registration.",
                   InfoBarSeverity.Informational);
        var error = new StringBuilder(600);
        bool ok = await Task.Run(() => Native.xrfps_uninstall(error, error.Capacity) != 0);
        SetInstallBusy(false);
        RefreshStatus();
        ShowAction(ok ? "Layer uninstalled" : "Uninstall failed",
                   ok ? "OpenXR apps started later will no longer load xrFPS." : error.ToString(),
                   ok ? InfoBarSeverity.Success : InfoBarSeverity.Error);
    }

    private async void RestoreDefaults_Click(object sender, RoutedEventArgs e)
    {
        var dialog = new ContentDialog
        {
            XamlRoot = RootGrid.XamlRoot,
            Title = "Restore all HUD settings?",
            Content = "Gesture, placement, appearance, and readout choices will return to their defaults.",
            PrimaryButtonText = "Restore defaults",
            CloseButtonText = "Cancel",
            DefaultButton = ContentDialogButton.Close,
        };
        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        Native.xrfps_config_defaults(out _config);
        ApplyConfigToControls();
        bool saved = Native.xrfps_config_save(ref _config) != 0;
        RenderPreview();
        ShowAction(saved ? "Defaults restored" : "Defaults restored but not saved",
                   saved ? "The running layer will pick up the reset automatically."
                         : "Check that the xrFPS settings folder is writable.",
                   saved ? InfoBarSeverity.Success : InfoBarSeverity.Error);
    }

    private void OpenFolder_Click(object sender, RoutedEventArgs e)
    {
        string dir = Path.GetDirectoryName(ManifestPathText.Text) ?? "";
        if (Directory.Exists(dir))
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(dir) { UseShellExecute = true });
        }
        else
        {
            ShowAction("Folder not found", "Install the layer first to create its local folder.",
                       InfoBarSeverity.Warning);
        }
    }

    private void OpenLog_Click(object sender, RoutedEventArgs e)
    {
        string dir = Path.GetDirectoryName(ManifestPathText.Text) ?? "";
        string log = Path.Combine(dir, "xrfps.log");
        if (File.Exists(log))
        {
            System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(log) { UseShellExecute = true });
        }
        else
        {
            ShowAction("No log yet", "The log appears after an OpenXR app loads the layer.",
                       InfoBarSeverity.Informational);
        }
    }

    // ---- updates -----------------------------------------------------

    private async Task CheckForUpdatesAsync(bool userInitiated)
    {
        if (userInitiated)
        {
            UpdateRing.IsActive = true;
            CheckUpdateButton.IsEnabled = false;
            UpdateStatusText.Text = "Checking…";
        }

        UpdateInfo? update = await _updates.CheckAsync(_shutdown.Token);

        UpdateRing.IsActive = false;
        CheckUpdateButton.IsEnabled = true;

        if (update is null)
        {
            // A failed check (offline, rate-limited) must never nag; only a
            // deliberate check reports that nothing was found.
            UpdateStatusText.Text = userInitiated ? "You are on the latest version." : "";
            return;
        }

        UpdateStatusText.Text = $"Version {update.Version} is available.";

        var dialog = new ContentDialog
        {
            XamlRoot = RootGrid.XamlRoot,
            Title = $"Update available — {update.Name}",
            PrimaryButtonText = "Update now",
            CloseButtonText = "Later",
            DefaultButton = ContentDialogButton.Primary,
            Content = new ScrollViewer
            {
                MaxHeight = 340,
                Content = new StackPanel
                {
                    Spacing = 10,
                    Children =
                    {
                        new TextBlock
                        {
                            Text = $"xrFPS {update.Version} is available. You have {AppUpdateService.CurrentVersion}.",
                            TextWrapping = TextWrapping.Wrap,
                        },
                        new TextBlock
                        {
                            Text = update.Notes.Length > 1200 ? update.Notes[..1200] + "…" : update.Notes,
                            TextWrapping = TextWrapping.Wrap,
                            Opacity = 0.8,
                        },
                    },
                },
            },
        };

        if (await dialog.ShowAsync() != ContentDialogResult.Primary)
        {
            return;
        }

        UpdateRing.IsActive = true;
        CheckUpdateButton.IsEnabled = false;
        var progress = new Progress<double>(fraction =>
            UpdateStatusText.Text = $"Downloading… {fraction * 100:0}%");

        try
        {
            string? installer = await _updates.DownloadAsync(update, progress, _shutdown.Token);
            if (installer is null)
            {
                UpdateStatusText.Text = "Download failed.";
                ShowAction("Update failed", "The installer could not be downloaded. You can get it from GitHub Releases.",
                           InfoBarSeverity.Error);
                return;
            }

            if (AppUpdateService.StartUpdateAndRelaunch(installer))
            {
                UpdateStatusText.Text = "Installing…";
                await Task.Delay(500);
                Application.Current.Exit(); // setup replaces files this process holds
            }
            else
            {
                UpdateStatusText.Text = "The installer was not started.";
            }
        }
        catch (Exception ex)
        {
            UpdateStatusText.Text = "Update failed.";
            ShowAction("Update failed", ex.Message, InfoBarSeverity.Error);
        }
        finally
        {
            UpdateRing.IsActive = false;
            CheckUpdateButton.IsEnabled = true;
        }
    }

    private async void CheckUpdate_Click(object sender, RoutedEventArgs e) =>
        await CheckForUpdatesAsync(userInitiated: true);

    private void ShowAction(string title, string message, InfoBarSeverity severity)
    {
        ActionInfoBar.IsOpen = false;
        ActionInfoBar.Title = title;
        ActionInfoBar.Message = message;
        ActionInfoBar.Severity = severity;
        ActionInfoBar.IsOpen = true;
    }
}
