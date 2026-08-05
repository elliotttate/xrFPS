# Drives xrFPS's test-pose shared memory through a smooth upside-down
# figure-8 (position and orientation), 30 Hz, for the D3D12 angle test.
param(
    [double]$Seconds = 40
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @"
public struct QF { public double x, y, z, w;
    public QF(double x_, double y_, double z_, double w_) { x = x_; y = y_; z = z_; w = w_; }
    public static QF Mul(QF a, QF b) {
        return new QF(
            a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
            a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
            a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
            a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z);
    }
    public static QF AxisAngle(double ax, double ay, double az, double deg) {
        double r = deg * System.Math.PI / 180.0 / 2.0;
        double s = System.Math.Sin(r);
        return new QF(ax*s, ay*s, az*s, System.Math.Cos(r));
    }
}
"@ -ErrorAction SilentlyContinue

$mmf = [System.IO.MemoryMappedFiles.MemoryMappedFile]::CreateOrOpen('Local\xrFPS_TestPose', 64)
$acc = $mmf.CreateViewAccessor(0, 64)

$flip = [QF]::AxisAngle(0, 0, 1, 170)
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$frames = 0

try {
    while ($sw.Elapsed.TotalSeconds -lt $Seconds) {
        $t = $sw.Elapsed.TotalSeconds * 0.7  # one figure-8 every ~9 s
        $px = 0.22 * [math]::Sin($t)
        $py = -0.12 + 0.12 * [math]::Sin(2.0 * $t)
        $pz = -0.5

        $yaw = [QF]::AxisAngle(0, 1, 0, 30.0 * [math]::Sin($t))
        $pitch = [QF]::AxisAngle(1, 0, 0, 22.0 * [math]::Sin(2.0 * $t))
        $q = [QF]::Mul([QF]::Mul($yaw, $pitch), $flip)

        $acc.Write(4, [float]$px)
        $acc.Write(8, [float]$py)
        $acc.Write(12, [float]$pz)
        $acc.Write(16, [float]$q.x)
        $acc.Write(20, [float]$q.y)
        $acc.Write(24, [float]$q.z)
        $acc.Write(28, [float]$q.w)
        $acc.Write(0, [float]1.0)  # active flag last

        $frames++
        Start-Sleep -Milliseconds 33
    }
} finally {
    $acc.Write(0, [float]0.0)  # deactivate: controller "unflips"
    $acc.Dispose()
    $mmf.Dispose()
}
Write-Output "FIGURE-8 DONE ($frames pose updates)"
