using System.Runtime.InteropServices;

namespace xrFPS.ControlCenter;

// Flat mirror of XrFpsConfigAbi in src/core_api.h. Field order and types
// must match exactly.
[StructLayout(LayoutKind.Sequential)]
public struct NativeConfig
{
    public int Enabled;
    public int Hand;
    public float FlipAngleDeg;
    public float ShowDelaySec;
    public float HideDelaySec;
    public int F10Toggle;

    public float PanelWidthM;
    public float PanelOffsetM;
    public float PanelRollDeg;

    public float Opacity;
    public float HudRefreshHz;
    public int Theme;
    public int ShowGraph;
    public int GraphHeight;

    public int ShowFps;
    public int ShowFrametime;
    public int ShowTarget;
    public int ShowOnTarget;
    public int ShowPercentiles;
    public int ShowCpuUsage;
    public int ShowGpuUsage;
    public int ShowGpuAppUsage;
    public int ShowVram;
    public int ShowGpuTemp;
    public int ShowCpuTemp;
    public int ShowGpuClock;
    public int ShowRam;
}

[StructLayout(LayoutKind.Sequential)]
public struct NativeMetrics
{
    public float CpuUsagePct;
    public float GpuUsagePct;
    public float GpuAppUsagePct;
    public float VramUsedMb;
    public float VramTotalMb;
    public float GpuTempC;
    public float CpuTempC;
    public float GpuClockMhz;
    public float RamUsedGb;
    public float RamTotalGb;
    public uint AppPid;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
    public string AppName;
    public int Running;
}

// Direct access to a WinRT IBuffer's bytes, so the HUD pixels can be copied
// into a WriteableBitmap without an intermediate stream.
[ComImport]
[Guid("905a0fef-bc53-11df-8c49-001e4fc686da")]
[InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal unsafe interface IBufferByteAccess
{
    void Buffer(out byte* buffer);
}

internal static class Native
{
    private const string Dll = "xrfps_core.dll";

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void xrfps_config_defaults(out NativeConfig config);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int xrfps_config_load(out NativeConfig config);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int xrfps_config_save(ref NativeConfig config);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern void xrfps_config_path(System.Text.StringBuilder buffer, int length);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int xrfps_hud_width();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int xrfps_hud_height();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void xrfps_hud_render(ref NativeConfig config, int useLiveMetrics,
                                                 float previewTargetMs, byte[] outBgra);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern int xrfps_status(System.Text.StringBuilder manifestPath, int length);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern int xrfps_install(System.Text.StringBuilder error, int length);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern int xrfps_uninstall(System.Text.StringBuilder error, int length);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern int xrfps_metrics_start();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void xrfps_metrics_stop();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void xrfps_metrics_sample();

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl)]
    public static extern void xrfps_metrics_get(out NativeMetrics metrics);

    [DllImport(Dll, CallingConvention = CallingConvention.Cdecl, CharSet = CharSet.Ansi)]
    public static extern void xrfps_metrics_status(System.Text.StringBuilder buffer, int length);
}
