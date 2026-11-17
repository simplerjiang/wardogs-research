$src = @'
using System;
using System.Runtime.InteropServices;

public static class Qws {
    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr VirtualAlloc(IntPtr lpAddress, UIntPtr dwSize, uint flAllocationType, uint flProtect);

    [DllImport("psapi.dll", SetLastError = true)]
    public static extern bool QueryWorkingSetEx(IntPtr hProcess, [In, Out] UIntPtr[] pv, uint cb);

    [StructLayout(LayoutKind.Sequential)]
    public struct WsEx { public UIntPtr VirtualAddress; public UIntPtr Flags; }

    public static string Probe() {
        IntPtr hProc = (IntPtr)(-1);
        IntPtr block = VirtualAlloc(IntPtr.Zero, (UIntPtr)(0x4000 * 4), 0x3000, 0x04);
        if (block == IntPtr.Zero) return "VirtualAlloc failed";
        for (int i = 0; i < 0x4000 * 4; i += 0x1000) Marshal.WriteByte(block, i, 0x41);
        var sb = new System.Text.StringBuilder();
        for (int p = 0; p < 4; ++p) {
            var arr = new UIntPtr[2];
            arr[0] = (UIntPtr)((long)block + p * 0x1000);
            arr[1] = UIntPtr.Zero;
            bool ok = QueryWorkingSetEx(hProc, arr, 16);
            ulong flags = (ulong)arr[1];
            ulong pfn12 = (flags >> 12) & 0xFFFFFFFFFFUL;
            sb.AppendLine(string.Format("va=0x{0:X} ok={1} flags=0x{2:X} pfn(>>12)=0x{3:X} pa=0x{4:X}",
                (long)block + p * 0x1000, ok, flags, pfn12, pfn12 << 12));
        }
        return sb.ToString();
    }
}
'@
Add-Type -TypeDefinition $src -Language CSharp
[Qws]::Probe()
