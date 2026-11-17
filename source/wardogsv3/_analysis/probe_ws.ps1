$src = @'
using System;
using System.Runtime.InteropServices;

public static class Probe
{
    [DllImport("ntdll.dll")]
    public static extern int NtQuerySystemInformation(int cls, IntPtr buf, int len, out int ret);

    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(int access, bool inherit, int pid);

    [DllImport("psapi.dll", SetLastError=true)]
    public static extern bool QueryWorkingSetEx(IntPtr hProcess, IntPtr pv, int cb);

    [DllImport("kernel32.dll")]
    public static extern IntPtr GetCurrentProcess();

    public static string Modules()
    {
        int len = 0; int ret;
        NtQuerySystemInformation(11, IntPtr.Zero, 0, out ret);
        len = ret + 0x10000;
        IntPtr buf = Marshal.AllocHGlobal(len);
        int st = NtQuerySystemInformation(11, buf, len, out ret);
        if (st != 0) { Marshal.FreeHGlobal(buf); return "STATUS 0x" + st.ToString("X8"); }
        int count = Marshal.ReadInt32(buf);
        var sb = new System.Text.StringBuilder();
        sb.AppendLine("count=" + count);
        IntPtr p = (IntPtr)((long)buf + 8);
        for (int i = 0; i < count && i < 8; i++)
        {
            long imageBase = Marshal.ReadInt64(p);
            int imageSize = Marshal.ReadInt32(p + 0x30);
            IntPtr namePtr = Marshal.ReadIntPtr(p + 0x38);
            string name = Marshal.PtrToStringUni(namePtr);
            sb.AppendLine(string.Format("0x{0:X16} size=0x{1:X} {2}", imageBase, imageSize, name));
            p = (IntPtr)((long)p + 0x40 + 0x100);
        }
        Marshal.FreeHGlobal(buf);
        return sb.ToString();
    }

    public static string WorkingSet(long va)
    {
        IntPtr h = GetCurrentProcess();
        IntPtr buf = Marshal.AllocHGlobal(16);
        for (int i = 0; i < 16; i++) Marshal.WriteByte(buf, i, 0);
        Marshal.WriteInt64(buf, va);
        bool ok = QueryWorkingSetEx(h, buf, 16);
        long flags = Marshal.ReadInt64(buf + 8);
        long virt = Marshal.ReadInt64(buf + 8);
        Marshal.FreeHGlobal(buf);
        return string.Format("ok={0} err={1} value=0x{2:X16} pfnPart=0x{3:X} valid={4}",
            ok, Marshal.GetLastWin32Error(), virt, (virt >> 12) & 0xFFFFFFFFF, (virt & 1));
    }
}
'@
Add-Type -TypeDefinition $src -ReferencedAssemblies 'System.Runtime','System.Runtime.InteropServices' -ErrorAction Stop
"--- modules ---"
[Probe]::Modules()
"--- workset of our own heap page ---"
$b = [byte[]]::new(4096)
$h = [System.Runtime.InteropServices.GCHandle]::Alloc($b, 'Pinned')
[Probe]::WorkingSet($h.AddrOfPinnedObject().ToInt64())
"--- workset of kernel-ish VA ---"
[Probe]::WorkingSet([int64]0xFFFFF78000000000)
[Probe]::WorkingSet([int64]0xFFFFF80000000000)
$h.Free()