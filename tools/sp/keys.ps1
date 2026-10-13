<#
.SYNOPSIS
    Bring the game's window to the front and send it keystrokes (the front-end menu takes no gamepad from the mod).

    Windows refuses SetForegroundWindow to a background process unless it was the last to get input: an Alt press
    first (the usual workaround), then the window is checked to be in front before any key is sent (SendKeys types into
    whatever window is in front; 2026-10-13 a session sat at the title screen because Enter went elsewhere).

.EXAMPLE
    tools\sp\keys.ps1 '{ENTER}'
    tools\sp\keys.ps1 '{DOWN}','{ENTER}' -DelayMs 800
#>
param(
    [Parameter(Mandatory)] [string[]] $Keys,
    [int] $DelayMs = 500
)
$ErrorActionPreference = 'Stop'
$p = Get-Process -Name RDR -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $p) { throw 'RDR.exe is not running (or has no window)' }
if (-not ('RdrvrWin' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class RdrvrWin {
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
'@
}
$h = $p.MainWindowHandle
$front = $false
for ($i = 0; $i -lt 5 -and -not $front; ++$i) {
    if ([RdrvrWin]::IsIconic($h)) { [void][RdrvrWin]::ShowWindow($h, 9) }  # SW_RESTORE
    [RdrvrWin]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero)  # Alt down / up: lets this process move the foreground
    [RdrvrWin]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
    [void][RdrvrWin]::SetForegroundWindow($h)
    Start-Sleep -Milliseconds 200
    $front = [RdrvrWin]::GetForegroundWindow() -eq $h
}
if (-not $front) { throw 'could not bring the game window to the front' }
# The game reads the keyboard by scan code (DirectInput): SendKeys' virtual keys do nothing in its front end
# (2026-10-13), so each key is a scan-code press held 60 ms (SendInput, KEYEVENTF_SCANCODE).
if (-not ('RdrvrKeys' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Threading;
public static class RdrvrKeys {
    [StructLayout(LayoutKind.Sequential)] struct KEYBDINPUT { public ushort vk, scan; public uint flags, time; public IntPtr extra; }
    [StructLayout(LayoutKind.Explicit, Size = 40)] struct INPUT { [FieldOffset(0)] public uint type; [FieldOffset(8)] public KEYBDINPUT ki; }
    [DllImport("user32.dll", SetLastError = true)] static extern uint SendInput(uint n, INPUT[] inputs, int size);
    static void Send(ushort scan, bool ext, bool up) {
        var i = new INPUT[1];
        i[0].type = 1;  // INPUT_KEYBOARD
        i[0].ki.scan = scan;
        i[0].ki.flags = 0x8u | (ext ? 0x1u : 0u) | (up ? 0x2u : 0u);  // SCANCODE | EXTENDEDKEY | KEYUP
        if (SendInput(1, i, Marshal.SizeOf(typeof(INPUT))) != 1) throw new Exception("SendInput failed " + Marshal.GetLastWin32Error());
    }
    public static void Press(ushort scan, bool ext) { Send(scan, ext, false); Thread.Sleep(60); Send(scan, ext, true); }
}
'@
}
$scan = @{ '{ENTER}' = @(0x1C, $false); '{ESC}' = @(0x01, $false); '{DOWN}' = @(0x50, $true); '{UP}' = @(0x48, $true);
           '{LEFT}' = @(0x4B, $true); '{RIGHT}' = @(0x4D, $true); ' ' = @(0x39, $false); 'z' = @(0x2C, $false) }
foreach ($k in $Keys) {
    if ([RdrvrWin]::GetForegroundWindow() -ne $h) { throw 'the game window lost the front before a key' }
    if (-not $scan.ContainsKey($k)) { throw "no scan code for $k" }
    [RdrvrKeys]::Press([uint16]$scan[$k][0], [bool]$scan[$k][1])
    Start-Sleep -Milliseconds $DelayMs
}
"sent $($Keys -join ' ')"
