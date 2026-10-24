using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
public sealed class QbtcPanelJob : IDisposable {
    [StructLayout(LayoutKind.Sequential)] struct Basic { public long ProcessTime, JobTime; public uint Flags; public UIntPtr MinWorking, MaxWorking; public uint Active; public UIntPtr Affinity; public uint Priority, Scheduling; }
    [StructLayout(LayoutKind.Sequential)] struct Counters { public ulong ReadOps, WriteOps, OtherOps, ReadBytes, WriteBytes, OtherBytes; }
    [StructLayout(LayoutKind.Sequential)] struct Limits { public Basic Basic; public Counters IO; public UIntPtr ProcessMemory, JobMemory, PeakProcess, PeakJob; }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr CreateJobObject(IntPtr attributes,string name);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool SetInformationJobObject(IntPtr job,int type,ref Limits limits,uint size);
    [DllImport("kernel32.dll", SetLastError=true)] static extern bool AssignProcessToJobObject(IntPtr job,IntPtr process);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    IntPtr handle;
    public QbtcPanelJob() {
        handle=CreateJobObject(IntPtr.Zero,null);
        if(handle==IntPtr.Zero) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        Limits limits=new Limits(); limits.Basic.Flags=0x2000;
        if(!SetInformationJobObject(handle,9,ref limits,(uint)Marshal.SizeOf(typeof(Limits)))) { int error=Marshal.GetLastWin32Error(); Dispose(); throw new System.ComponentModel.Win32Exception(error); }
    }
    public void Add(Process process) { if(!AssignProcessToJobObject(handle,process.Handle)) throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error()); }
    public void Dispose() { if(handle!=IntPtr.Zero) { CloseHandle(handle); handle=IntPtr.Zero; } }
}
