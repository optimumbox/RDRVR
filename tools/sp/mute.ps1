<#
.SYNOPSIS
    Mute (or unmute) a process's audio sessions on every playback device: the game's sound in test sessions, the rest of
    the PC's audio untouched (Windows' per-app volume, as the Volume Mixer sets it).

.EXAMPLE
    tools\sp\mute.ps1            # mute RDR
    tools\sp\mute.ps1 -Unmute
#>
param(
    [string] $Process = 'RDR',
    [switch] $Unmute
)
$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

namespace RdrvrAudio {
    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator {
        int EnumAudioEndpoints(int dataFlow, int stateMask, out IMMDeviceCollection devices);
    }
    [Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceCollection {
        int GetCount(out int count);
        int Item(int index, out IMMDevice device);
    }
    [Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice {
        int Activate(ref Guid iid, int clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
    }
    [Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionManager2 {
        int GetAudioSessionControl(IntPtr guid, int flags, out IntPtr ctl);
        int GetSimpleAudioVolume(IntPtr guid, int flags, out IntPtr vol);
        int GetSessionEnumerator(out IAudioSessionEnumerator e);
    }
    [Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionEnumerator {
        int GetCount(out int count);
        int GetSession(int index, out IAudioSessionControl2 session);
    }
    [Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IAudioSessionControl2 {
        // IAudioSessionControl
        int GetState(out int state);
        int GetDisplayName(out IntPtr name);
        int SetDisplayName(IntPtr name, IntPtr ctx);
        int GetIconPath(out IntPtr path);
        int SetIconPath(IntPtr path, IntPtr ctx);
        int GetGroupingParam(out Guid g);
        int SetGroupingParam(ref Guid g, IntPtr ctx);
        int RegisterAudioSessionNotification(IntPtr n);
        int UnregisterAudioSessionNotification(IntPtr n);
        // IAudioSessionControl2
        int GetSessionIdentifier(out IntPtr id);
        int GetSessionInstanceIdentifier(out IntPtr id);
        int GetProcessId(out uint pid);
    }
    [Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface ISimpleAudioVolume {
        int SetMasterVolume(float level, ref Guid ctx);
        int GetMasterVolume(out float level);
        int SetMute(int mute, ref Guid ctx);
        int GetMute(out int mute);
    }
    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")] class MMDeviceEnumerator { }

    public static class Mixer {
        // the number of sessions of these processes set
        public static int SetMute(uint[] pids, bool mute) {
            var en = (IMMDeviceEnumerator)new MMDeviceEnumerator();
            IMMDeviceCollection devs;
            en.EnumAudioEndpoints(0 /* render */, 1 /* active */, out devs);
            int n, done = 0;
            devs.GetCount(out n);
            var iid = typeof(IAudioSessionManager2).GUID;
            var ctx = Guid.Empty;
            for (int i = 0; i < n; ++i) {
                IMMDevice dev;
                devs.Item(i, out dev);
                object o;
                if (dev.Activate(ref iid, 1 /* CLSCTX_INPROC_SERVER */, IntPtr.Zero, out o) != 0) continue;
                IAudioSessionEnumerator se;
                ((IAudioSessionManager2)o).GetSessionEnumerator(out se);
                int m;
                se.GetCount(out m);
                for (int j = 0; j < m; ++j) {
                    IAudioSessionControl2 s;
                    se.GetSession(j, out s);
                    uint pid;
                    s.GetProcessId(out pid);
                    if (Array.IndexOf(pids, pid) < 0) continue;
                    ((ISimpleAudioVolume)s).SetMute(mute ? 1 : 0, ref ctx);
                    ++done;
                }
            }
            return done;
        }
    }
}
'@

$procs = @(Get-Process -Name $Process -ErrorAction SilentlyContinue)
if (-not $procs) { "no $Process process"; exit 1 }
$n = [RdrvrAudio.Mixer]::SetMute([uint32[]]($procs | ForEach-Object { [uint32]$_.Id }), -not $Unmute)
if ($n -eq 0) { "no audio session of $Process yet (it opens with the game's sound)"; exit 1 }
"$Process $(if ($Unmute) { 'unmuted' } else { 'muted' }): $n session(s)"
