# console_inject.ps1 —— 往运行中的 main.exe 控制台注入命令行
#
# 为什么需要它：`--console-command` 的排队命令是**一次性**提交的
# （ConsoleSubsystem::QueueLaunchCommands 在 delay 帧后把整批交给 DebugConsole），
# 而 `screenshot` 在 RenderSystem 里只保留**一个**待处理路径（后一条覆盖前一条）。
# 因此"同一进程内先 loadworld 切场景、再分别截图"无法用启动参数表达。
#
# DebugConsole 的交互输入走 _kbhit/_getch（conio），读的是控制台输入缓冲区，
# 不理会 stdin 重定向；所以这里用 AttachConsole + WriteConsoleInput 直接往
# 目标进程的控制台输入缓冲区写按键事件，等价于人工敲入一行并回车。
#
# **为什么必须另起进程**：AttachConsole/FreeConsole 会改动调用进程的标准句柄状态，
# 之后再从同一个进程里启动子进程（例如 py.exe）会报
# "The Win32 internal error ... occurred while getting the console mode"。
# 所以 `Send-ConsoleLine` 总是起一个一次性的 pwsh 子进程做注入，宿主进程的
# 句柄与输出重定向不受影响。
#
# 用法（点源，推荐）：
#   . .\tool\validation\console_inject.ps1
#   Send-ConsoleLine -ProcessId 1234 -Line 'screenshot foo.bmp'
#
# 用法（直接调用，内部使用）：
#   pwsh -NoProfile -File .\tool\validation\console_inject.ps1 -InjectProcessId 1234 -InjectLine 'help'

[CmdletBinding()]
param(
    [int]$InjectProcessId = 0,
    [string]$InjectLine = ''
)

Set-StrictMode -Version Latest

# 点源时 $PSCommandPath 就是本文件路径；函数在被调用时 $PSScriptRoot 可能已经失效，
# 所以在这里先把它固化到脚本作用域。
$script:ConsoleInjectScriptPath = $PSCommandPath
$script:PowerShellExePath = if (Test-Path (Join-Path $PSHOME 'pwsh.exe')) {
    Join-Path $PSHOME 'pwsh.exe'
} else {
    'pwsh'
}

if (-not ('VlConsoleInject' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class VlConsoleInject
{
    [StructLayout(LayoutKind.Explicit, CharSet = CharSet.Unicode)]
    public struct KEY_EVENT_RECORD
    {
        [FieldOffset(0)]  public int bKeyDown;
        [FieldOffset(4)]  public ushort wRepeatCount;
        [FieldOffset(6)]  public ushort wVirtualKeyCode;
        [FieldOffset(8)]  public ushort wVirtualScanCode;
        [FieldOffset(10)] public char UnicodeChar;
        [FieldOffset(12)] public uint dwControlKeyState;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct INPUT_RECORD
    {
        [FieldOffset(0)] public ushort EventType;
        [FieldOffset(4)] public KEY_EVENT_RECORD KeyEvent;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool FreeConsole();

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool AttachConsole(uint dwProcessId);

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern IntPtr CreateFileW(
        string lpFileName, uint dwDesiredAccess, uint dwShareMode,
        IntPtr lpSecurityAttributes, uint dwCreationDisposition,
        uint dwFlagsAndAttributes, IntPtr hTemplateFile);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteConsoleInputW(
        IntPtr hConsoleInput, INPUT_RECORD[] lpBuffer, uint nLength,
        out uint lpNumberOfEventsWritten);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr hObject);

    private const uint GENERIC_READ_WRITE = 0xC0000000u;
    private const uint FILE_SHARE_READ_WRITE = 0x00000003u;
    private const uint OPEN_EXISTING = 3u;
    private const ushort KEY_EVENT = 0x0001;

    // 返回空字符串表示成功，否则是失败原因。
    public static string Send(uint processId, string text)
    {
        FreeConsole();
        if (!AttachConsole(processId))
        {
            return "AttachConsole failed, win32 error " + Marshal.GetLastWin32Error();
        }

        string failure = string.Empty;
        IntPtr consoleInput = CreateFileW(
            "CONIN$", GENERIC_READ_WRITE, FILE_SHARE_READ_WRITE,
            IntPtr.Zero, OPEN_EXISTING, 0, IntPtr.Zero);
        if (consoleInput == IntPtr.Zero || consoleInput == new IntPtr(-1))
        {
            failure = "CreateFile(CONIN$) failed, win32 error " + Marshal.GetLastWin32Error();
        }
        else
        {
            INPUT_RECORD[] records = new INPUT_RECORD[text.Length * 2];
            for (int index = 0; index < text.Length; ++index)
            {
                records[index * 2].EventType = KEY_EVENT;
                records[index * 2].KeyEvent.bKeyDown = 1;
                records[index * 2].KeyEvent.wRepeatCount = 1;
                records[index * 2].KeyEvent.UnicodeChar = text[index];

                records[index * 2 + 1].EventType = KEY_EVENT;
                records[index * 2 + 1].KeyEvent.bKeyDown = 0;
                records[index * 2 + 1].KeyEvent.wRepeatCount = 1;
                records[index * 2 + 1].KeyEvent.UnicodeChar = text[index];
            }

            uint written;
            if (!WriteConsoleInputW(consoleInput, records, (uint)records.Length, out written))
            {
                failure = "WriteConsoleInput failed, win32 error " + Marshal.GetLastWin32Error();
            }
            CloseHandle(consoleInput);
        }

        FreeConsole();
        return failure;
    }
}
'@
}

function Send-ConsoleLine {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][int]$ProcessId,
        [Parameter(Mandatory)][string]$Line
    )

    # 注入必须发生在一次性子进程里：AttachConsole/FreeConsole 会污染宿主进程的
    # 标准句柄，导致宿主之后启动任何子进程都可能失败。
    $output = & $script:PowerShellExePath -NoProfile -File $script:ConsoleInjectScriptPath `
        -InjectProcessId $ProcessId -InjectLine $Line 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "console injection failed (exit $LASTEXITCODE): $output"
    }
}

if ($InjectProcessId -ne 0) {
    $failure = [VlConsoleInject]::Send([uint32]$InjectProcessId, $InjectLine + "`r")
    if ($failure -ne '') {
        Write-Error $failure
        exit 1
    }
    exit 0
}
