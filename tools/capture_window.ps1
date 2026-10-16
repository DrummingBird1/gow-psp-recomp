# Captures a single window (by process id) to a PNG using PrintWindow, so only that
# window is recorded, never the rest of the desktop.
# Usage: capture_window.ps1 (-ProcessId <pid> | -Title <exact window title>) -Out <file.png>
param([int]$ProcessId = 0, [string]$Title = '', [Parameter(Mandatory)][string]$Out)

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class Win {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    public static IntPtr Find(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, p) => { uint w; GetWindowThreadProcessId(h, out w);
            if (w == pid && IsWindowVisible(h)) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
"@
[Win]::SetProcessDPIAware() | Out-Null
$h = if ($Title) { [Win]::FindWindow($null, $Title) } else { [Win]::Find([uint32]$ProcessId) }
if ($h -eq [IntPtr]::Zero) { throw "no visible window for '$Title' / process $ProcessId" }
$r = New-Object Win+RECT
[Win]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hgt = $r.B - $r.T
# 24-bit: the window's own alpha (0 where the game left framebuffer alpha at 0, e.g. movie pictures)
# would otherwise make those pixels transparent in the PNG
$bmp = New-Object System.Drawing.Bitmap $w, $hgt, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
[Win]::PrintWindow($h, $hdc, 2) | Out-Null   # PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc); $g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
"captured ${w}x${hgt} -> $Out"
