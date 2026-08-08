using System;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;

namespace xrFPS.ControlCenter.Services
{
    [StructLayout(LayoutKind.Sequential)]
    public struct CaptureHeader
    {
        public uint Width;
        public uint Height;
        public uint Stride;
        public uint Sequence;
        public uint Active;
    }

    public class WindowCaptureService : IDisposable
    {
        private readonly IntPtr _hwnd;
        private readonly int _maxWidth;
        private readonly int _fps;
        private readonly CancellationTokenSource _cts = new();
        private Task? _captureTask;

        private MemoryMappedFile? _mmf;
        private MemoryMappedViewAccessor? _accessor;
        private uint _sequence = 0;

        // --- GDI / Win32 P/Invokes ---
        [StructLayout(LayoutKind.Sequential)]
        private struct RECT
        {
            public int left;
            public int top;
            public int right;
            public int bottom;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct BITMAPINFOHEADER
        {
            public uint biSize;
            public int biWidth;
            public int biHeight;
            public ushort biPlanes;
            public ushort biBitCount;
            public uint biCompression;
            public uint biSizeImage;
            public int biXPelsPerMeter;
            public int biYPelsPerMeter;
            public uint biClrUsed;
            public uint biClrImportant;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct BITMAPINFO
        {
            public BITMAPINFOHEADER bmiHeader;
            public int bmiColors; // Place-holder for single color dword
        }

        [DllImport("user32.dll")]
        private static extern bool GetWindowRect(IntPtr hWnd, ref RECT lpRect);

        [DllImport("user32.dll")]
        private static extern bool IsWindow(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern bool IsIconic(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern IntPtr GetDC(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);

        [DllImport("gdi32.dll")]
        private static extern IntPtr CreateCompatibleDC(IntPtr hdc);

        [DllImport("gdi32.dll")]
        private static extern IntPtr CreateCompatibleBitmap(IntPtr hdc, int cx, int cy);

        [DllImport("gdi32.dll")]
        private static extern IntPtr SelectObject(IntPtr hdc, IntPtr hgdiobj);

        [DllImport("gdi32.dll")]
        private static extern bool DeleteObject(IntPtr ho);

        [DllImport("gdi32.dll")]
        private static extern bool DeleteDC(IntPtr hdc);

        [DllImport("user32.dll")]
        private static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);

        [DllImport("gdi32.dll")]
        private static extern int SetStretchBltMode(IntPtr hdc, int mode);

        [DllImport("gdi32.dll")]
        private static extern bool StretchBlt(
            IntPtr hdcDest, int xDest, int yDest, int wDest, int hDest,
            IntPtr hdcSrc, int xSrc, int ySrc, int wSrc, int hSrc,
            uint rop
        );

        [DllImport("gdi32.dll")]
        private static extern int GetDIBits(
            IntPtr hdc, IntPtr hbmp, uint uStartScan, uint cScanLines,
            byte[] lpvBits, ref BITMAPINFO lpbmi, uint uUsage
        );

        private const string MemoryMappedFileName = "Local\\xrFPS_CaptureFrame";
        private const uint PW_RENDERFULLCONTENT = 2;
        private const uint SRCCOPY = 0x00CC0020;
        private const int COLORONCOLOR = 3;

        // Size matches kCaptureMemorySize in capture_shared.h: sizeof(CaptureHeader) + (2048 * 2048 * 4)
        private const int MaxWidthLimit = 2048;
        private const int MaxHeightLimit = 2048;
        private const int MemoryMappedFileSize = 20 + (MaxWidthLimit * MaxHeightLimit * 4);

        public WindowCaptureService(IntPtr hwnd, int maxWidth, int fps)
        {
            _hwnd = hwnd;
            _maxWidth = Math.Clamp(maxWidth, 64, MaxWidthLimit);
            _fps = Math.Clamp(fps, 5, 90);
        }

        public void Start()
        {
            try
            {
                _mmf = MemoryMappedFile.CreateOrOpen(MemoryMappedFileName, MemoryMappedFileSize, MemoryMappedFileAccess.ReadWrite);
                _accessor = _mmf.CreateViewAccessor();
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to create memory mapped file: {ex.Message}");
                return;
            }

            _captureTask = Task.Run(CaptureLoop);
        }

        public void Stop()
        {
            _cts.Cancel();
            try
            {
                _captureTask?.Wait();
            }
            catch
            {
                // Ignore task cancellation exceptions
            }

            CleanupSharedMemory(active: 0);
            Dispose();
        }

        private async Task CaptureLoop()
        {
            int intervalMs = 1000 / _fps;

            while (!_cts.IsCancellationRequested)
            {
                var startTime = DateTime.UtcNow;

                if (IsWindow(_hwnd) && !IsIconic(_hwnd))
                {
                    CaptureFrame();
                }

                int elapsed = (int)(DateTime.UtcNow - startTime).TotalMilliseconds;
                int delay = Math.Max(1, intervalMs - elapsed);
                try
                {
                    await Task.Delay(delay, _cts.Token);
                }
                catch (TaskCanceledException)
                {
                    break;
                }
            }
        }

        private void CaptureFrame()
        {
            RECT rect = new RECT();
            if (!GetWindowRect(_hwnd, ref rect)) return;

            int w = rect.right - rect.left;
            int h = rect.bottom - rect.top;
            if (w <= 0 || h <= 0) return;

            // Constrain target size while preserving aspect ratio
            int targetW = w;
            int targetH = h;
            if (targetW > _maxWidth)
            {
                targetW = _maxWidth;
                targetH = (int)((double)h * _maxWidth / w);
            }

            // Ensure width is a multiple of 64 to guarantee 256-byte D3D12 alignment (4 bytes per pixel)
            targetW = (targetW / 64) * 64;
            if (targetW < 64) targetW = 64;
            targetH = (int)((double)h * targetW / w);
            if (targetH < 1) targetH = 1;

            if (targetW > MaxWidthLimit || targetH > MaxHeightLimit) return;

            IntPtr hdcScreen = GetDC(IntPtr.Zero);
            if (hdcScreen == IntPtr.Zero) return;

            IntPtr hdcWindow = CreateCompatibleDC(hdcScreen);
            IntPtr hbmWindow = CreateCompatibleBitmap(hdcScreen, w, h);
            IntPtr hOldWindow = SelectObject(hdcWindow, hbmWindow);

            // Capture the actual window contents (supporting hardware-accelerated composition)
            bool success = PrintWindow(_hwnd, hdcWindow, PW_RENDERFULLCONTENT);
            if (success)
            {
                IntPtr hdcScaled = CreateCompatibleDC(hdcScreen);
                IntPtr hbmScaled = CreateCompatibleBitmap(hdcScreen, targetW, targetH);
                IntPtr hOldScaled = SelectObject(hdcScaled, hbmScaled);

                SetStretchBltMode(hdcScaled, COLORONCOLOR);
                StretchBlt(hdcScaled, 0, 0, targetW, targetH, hdcWindow, 0, 0, w, h, SRCCOPY);

                byte[] pixelData = new byte[targetW * targetH * 4];
                BITMAPINFO bmi = new BITMAPINFO();
                bmi.bmiHeader.biSize = (uint)Marshal.SizeOf(typeof(BITMAPINFOHEADER));
                bmi.bmiHeader.biWidth = targetW;
                bmi.bmiHeader.biHeight = -targetH; // negative height for top-down BGRA format
                bmi.bmiHeader.biPlanes = 1;
                bmi.bmiHeader.biBitCount = 32;
                bmi.bmiHeader.biCompression = 0; // BI_RGB

                if (GetDIBits(hdcScaled, hbmScaled, 0, (uint)targetH, pixelData, ref bmi, 0) > 0)
                {
                    _sequence++;
                    CaptureHeader header = new CaptureHeader
                    {
                        Width = (uint)targetW,
                        Height = (uint)targetH,
                        Stride = (uint)(targetW * 4),
                        Sequence = _sequence,
                        Active = 1
                    };

                    WriteSharedFrame(header, pixelData);
                }

                SelectObject(hdcScaled, hOldScaled);
                DeleteObject(hbmScaled);
                DeleteDC(hdcScaled);
            }

            SelectObject(hdcWindow, hOldWindow);
            DeleteObject(hbmWindow);
            DeleteDC(hdcWindow);
            ReleaseDC(IntPtr.Zero, hdcScreen);
        }

        private void WriteSharedFrame(CaptureHeader header, byte[] pixelData)
        {
            if (_accessor == null) return;

            try
            {
                // Write header
                _accessor.Write(0, ref header);
                // Write pixels immediately following the header
                _accessor.WriteArray(20, pixelData, 0, pixelData.Length);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Failed to write to shared memory accessor: {ex.Message}");
            }
        }

        private void CleanupSharedMemory(uint active)
        {
            if (_accessor == null) return;
            try
            {
                CaptureHeader header = new CaptureHeader
                {
                    Width = 0,
                    Height = 0,
                    Stride = 0,
                    Sequence = 0,
                    Active = active
                };
                _accessor.Write(0, ref header);
            }
            catch
            {
                // Ignore errors during final shutdown
            }
        }

        public void Dispose()
        {
            _accessor?.Dispose();
            _accessor = null;
            _mmf?.Dispose();
            _mmf = null;
        }
    }
}
