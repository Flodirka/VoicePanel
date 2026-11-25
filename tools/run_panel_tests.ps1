# Runs the Qt panel tests under a hard resource budget, so a full run cannot
# take the desktop with it: one process at a time, a fixed set of cores, a
# memory cap enforced by a Job Object, a wall-clock timeout and a per-function
# timeout. The suite is slower by design; that is the point.
param(
    [string[]]$Filter = @(),
    [int]$MaxCores = 1,
    [int]$MemoryLimitMB = 768,
    [ValidateSet('Idle', 'BelowNormal', 'Normal')]
    [string]$Priority = 'BelowNormal',
    [int]$TimeoutSeconds = 1800,
    [int]$FunctionTimeoutSeconds = 600,
    [string]$LogPath = '',
    [switch]$Native,
    [switch]$SelfTest
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class VoicePanelTestJob
{
    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_BASIC_LIMIT_INFORMATION
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public UIntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct IO_COUNTERS
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
    {
        public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
        public IO_COUNTERS IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr CreateJobObject(IntPtr security, string name);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint length);

    [StructLayout(LayoutKind.Sequential)]
    public struct JOBOBJECT_BASIC_ACCOUNTING_INFORMATION
    {
        public long TotalUserTime;
        public long TotalKernelTime;
        public long ThisPeriodTotalUserTime;
        public long ThisPeriodTotalKernelTime;
        public uint TotalPageFaultCount;
        public uint TotalProcesses;
        public uint ActiveProcesses;
        public uint TotalTerminatedProcesses;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool QueryInformationJobObject(IntPtr job, int infoClass, IntPtr info,
                                                uint length, IntPtr returned);

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool CloseHandle(IntPtr handle);

    const int JobObjectBasicAccountingInformation = 1;
    const int JobObjectExtendedLimitInformation = 9;
    const uint JOB_OBJECT_LIMIT_PROCESS_MEMORY = 0x00000100;
    const uint JOB_OBJECT_LIMIT_JOB_MEMORY = 0x00000200;
    const uint JOB_OBJECT_LIMIT_ACTIVE_PROCESS = 0x00000008;
    const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;

    public static IntPtr Create(ulong memoryLimitBytes, uint activeProcessLimit)
    {
        IntPtr job = CreateJobObject(IntPtr.Zero, null);
        if (job == IntPtr.Zero)
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        info.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_JOB_MEMORY |
            JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        info.BasicLimitInformation.ActiveProcessLimit = activeProcessLimit;
        info.ProcessMemoryLimit = new UIntPtr(memoryLimitBytes);
        info.JobMemoryLimit = new UIntPtr(memoryLimitBytes);
        int length = Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
        IntPtr buffer = Marshal.AllocHGlobal(length);
        try
        {
            Marshal.StructureToPtr(info, buffer, false);
            if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, buffer, (uint)length))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
        return job;
    }

    public static void Assign(IntPtr job, IntPtr process)
    {
        if (!AssignProcessToJobObject(job, process))
            throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
    }

    // Peak working set of any process the job carried. Read after the run:
    // the Process object itself reports zeros once the child has exited.
    public static ulong PeakProcessBytes(IntPtr job)
    {
        var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
        int length = Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
        IntPtr buffer = Marshal.AllocHGlobal(length);
        try
        {
            Marshal.StructureToPtr(info, buffer, false);
            if (!QueryInformationJobObject(job, JobObjectExtendedLimitInformation, buffer,
                                           (uint)length, IntPtr.Zero))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            var result = (JOBOBJECT_EXTENDED_LIMIT_INFORMATION)Marshal.PtrToStructure(
                buffer, typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
            return result.PeakProcessMemoryUsed.ToUInt64();
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public static double CpuSeconds(IntPtr job)
    {
        var info = new JOBOBJECT_BASIC_ACCOUNTING_INFORMATION();
        int length = Marshal.SizeOf(typeof(JOBOBJECT_BASIC_ACCOUNTING_INFORMATION));
        IntPtr buffer = Marshal.AllocHGlobal(length);
        try
        {
            Marshal.StructureToPtr(info, buffer, false);
            if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, buffer,
                                           (uint)length, IntPtr.Zero))
                throw new System.ComponentModel.Win32Exception(Marshal.GetLastWin32Error());
            var result = (JOBOBJECT_BASIC_ACCOUNTING_INFORMATION)Marshal.PtrToStructure(
                buffer, typeof(JOBOBJECT_BASIC_ACCOUNTING_INFORMATION));
            return (result.TotalUserTime + result.TotalKernelTime) / 10000000.0;
        }
        finally
        {
            Marshal.FreeHGlobal(buffer);
        }
    }

    public static void Close(IntPtr job)
    {
        if (job != IntPtr.Zero) CloseHandle(job);
    }
}
"@

$oneMB = [uint64]1MB

if ($SelfTest) {
    # The cap is only real if an allocation above it fails. 128 MB holds a
    # shell comfortably; 512 MB cannot fit inside it.
    Write-Host 'self-test: a 512 MB allocation must fail under a 128 MB job cap'
    $job = [VoicePanelTestJob]::Create(128 * $oneMB, 1)
    $out = Join-Path $env:TEMP 'vp-selftest.out'
    $err = Join-Path $env:TEMP 'vp-selftest.err'
    $probe = Start-Process -FilePath 'powershell' -PassThru -NoNewWindow `
        -RedirectStandardOutput $out -RedirectStandardError $err `
        -ArgumentList @('-NoProfile', '-Command',
            'try { $null = New-Object byte[] (512MB); exit 0 } catch { exit 3 }')
    [VoicePanelTestJob]::Assign($job, $probe.Handle)
    $probe.WaitForExit(120000) | Out-Null
    $code = if ($probe.HasExited) { $probe.ExitCode } else { $null }
    [VoicePanelTestJob]::Close($job)
    if (-not $probe.HasExited) {
        Write-Host 'self-test FAILED: the probe did not finish'
        exit 1
    }
    if ($code -eq 0) {
        Write-Host 'self-test FAILED: an allocation above the cap succeeded'
        exit 1
    }
    Write-Host "self-test PASSED: allocation refused (exit $code)"
    exit 0
}

$exe = Join-Path $root 'panel\build\bin\VoicePanelTests.exe'
if (-not (Test-Path $exe)) {
    Write-Host "missing $exe; run tools\build_panel.ps1 first"
    exit 2
}

$log = if ($LogPath) { $LogPath } else { Join-Path $root 'runtime\logs\panel-tests.txt' }
if (-not [System.IO.Path]::IsPathRooted($log)) { $log = Join-Path $root $log }
New-Item -ItemType Directory -Force (Split-Path -Parent $log) | Out-Null
$consoleOut = "$log.console"
$consoleErr = "$log.console.err"

$qt = Join-Path $root 'runtime\Qt\6.8.3\msvc2022_64'
$env:PATH = (Join-Path $qt 'bin') + ';' + $env:PATH
$env:QT_QPA_PLATFORM_PLUGIN_PATH = (Join-Path $qt 'plugins\platforms')
$env:QTEST_FUNCTION_TIMEOUT = [string]($FunctionTimeoutSeconds * 1000)

$arguments = @('-o', "$log,txt")
if (-not $Native) { $arguments += @('-platform', 'offscreen') }
$arguments += $Filter

# One run at a time: two suites at once is exactly the load this script exists
# to prevent.
$mutex = New-Object System.Threading.Mutex($false, 'VoicePanelTestsRun')
if (-not $mutex.WaitOne(0)) {
    Write-Host 'another panel test run is already active'
    exit 3
}

$cores = [Math]::Max(1, [Math]::Min($MaxCores, [Environment]::ProcessorCount))
$mask = [int64]([Math]::Pow(2, $cores) - 1)
$job = [VoicePanelTestJob]::Create([uint64]$MemoryLimitMB * $oneMB, 1)

Write-Host ("budget: {0} core(s), {1} priority, {2} MB, {3} s, one process" -f `
    $cores, $Priority, $MemoryLimitMB, $TimeoutSeconds)
$started = Get-Date
$process = Start-Process -FilePath $exe -ArgumentList $arguments -PassThru -NoNewWindow `
    -WorkingDirectory $root -RedirectStandardOutput $consoleOut -RedirectStandardError $consoleErr
[VoicePanelTestJob]::Assign($job, $process.Handle)
$process.ProcessorAffinity = [IntPtr]$mask
$process.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::$Priority

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$peakBytes = [uint64]0
$cpuSeconds = 0.0
$timedOut = $false
while (-not $process.HasExited) {
    if ((Get-Date) -ge $deadline) { $timedOut = $true; break }
    try {
        $peakBytes = [Math]::Max($peakBytes, [uint64]$process.PeakWorkingSet64)
        $cpuSeconds = [Math]::Max($cpuSeconds, $process.TotalProcessorTime.TotalSeconds)
    } catch { }
    Start-Sleep -Milliseconds 100
}
if ($timedOut) {
    Write-Host "timeout after $TimeoutSeconds s; stopping the run"
    try { $process.Kill() } catch { }
    $process.WaitForExit(10000) | Out-Null
}
$elapsed = [int]((Get-Date) - $started).TotalSeconds
$code = if ($timedOut) { 124 } else { $process.ExitCode }
# The job keeps the true peak; a short run can end between two samples.
try { $peakBytes = [Math]::Max($peakBytes, [VoicePanelTestJob]::PeakProcessBytes($job)) } catch { }
try { $cpuSeconds = [Math]::Max($cpuSeconds, [VoicePanelTestJob]::CpuSeconds($job)) } catch { }
$peakMB = [int]($peakBytes / 1MB)
$cpuSeconds = [int]$cpuSeconds
[VoicePanelTestJob]::Close($job)
$mutex.ReleaseMutex()
$mutex.Dispose()

$totals = if (Test-Path $log) {
    Select-String -Path $log -Pattern '^Totals:' | Select-Object -Last 1
}
Write-Host ("wall {0} s, cpu {1} s, peak {2} MB, exit {3}" -f `
    $elapsed, $cpuSeconds, $peakMB, $code)
if ($totals) {
    Write-Host $totals.Line.Trim()
} else {
    Write-Host "no QtTest totals in $log; the run did not finish"
    if (Test-Path $consoleErr) {
        Get-Content $consoleErr -Tail 10 | ForEach-Object { Write-Host $_ }
    }
    if ($code -ne 124) {
        Write-Host 'if the process died at the memory cap, raise -MemoryLimitMB'
    }
}
exit $code
