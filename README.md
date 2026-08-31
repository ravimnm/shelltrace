# ShellTrace

**ShellTrace** is a lightweight Windows process-execution tracer written in C.

It runs a batch script, PowerShell script, or executable under the **Windows Debug API** and records the processes created during execution. Instead of only showing that a script succeeded or failed, ShellTrace exposes the underlying process activity, including **PIDs, parent-child relationships, executable paths, exit codes, execution duration, and the resulting process tree**.

---

## Why ShellTrace?

When a script fails, the final exit code often does not explain what actually happened.

For example:

```text
deploy.bat
    |
    +-- tool.exe
    |
    +-- powershell.exe
    |
    +-- cmd.exe
            |
            +-- failing.exe
```

A normal shell may only tell you:

```text
Process exited with code 7
```

ShellTrace instead reconstructs the process activity:

```text
cmd.exe [PID=8072, exit=0]
    PING.EXE [PID=8416, exit=0]
    powershell.exe [PID=5040, exit=0]
    cmd.exe [PID=9060, exit=0]
    cmd.exe [PID=11312, exit=7]
```

This makes process-level failures easier to inspect.

---

## Features

* **Process creation tracing**
* **Process termination tracing**
* **PID tracking**
* **Parent PID tracking**
* **Executable path detection**
* **Exit-code detection**
* **Execution-duration measurement**
* **Process-tree reconstruction**
* **Basic exception detection**
* Supports:

  * `.bat`
  * `.cmd`
  * `.ps1`
  * `.exe`

---

## Architecture

ShellTrace uses the Windows debugging API rather than repeatedly polling the process list.

```text
                    ShellTrace
                        |
                        v
                 CreateProcess()
                        |
                  DEBUG_PROCESS
                        |
                        v
                   Target
                        |
              +---------+---------+
              |                   |
              v                   v
          Child Process       Child Process
              |                   |
              +---------+---------+
                        |
                        v
                WaitForDebugEvent()
                        |
          +-------------+-------------+
          |             |             |
          v             v             v
       CREATE        EXCEPTION       EXIT
       PROCESS         EVENT        PROCESS
          |             |             |
          +-------------+-------------+
                        |
                        v
                Process Table
                        |
          +-------------+-------------+
          |             |             |
          v             v             v
         PID           PPID        Exit Code
                        |
                        v
                  Process Tree
```

---

## How It Works

### 1. Start the target process

ShellTrace launches the target using:

```c
CreateProcessA()
```

with:

```c
DEBUG_PROCESS
```

This places the target under Windows debugger control.

---

### 2. Wait for Windows debug events

ShellTrace enters an event loop using:

```c
WaitForDebugEvent()
```

Windows reports events such as:

```text
CREATE_PROCESS_DEBUG_EVENT
EXIT_PROCESS_DEBUG_EVENT
EXCEPTION_DEBUG_EVENT
CREATE_THREAD_DEBUG_EVENT
EXIT_THREAD_DEBUG_EVENT
LOAD_DLL_DEBUG_EVENT
UNLOAD_DLL_DEBUG_EVENT
```

ShellTrace currently focuses primarily on process lifecycle events.

---

### 3. Track process creation

When Windows reports:

```text
CREATE_PROCESS_DEBUG_EVENT
```

ShellTrace records:

* PID
* PPID
* executable name
* executable path
* start time

For example:

```text
[PROCESS START]
    PID      : 8416
    PPID     : 8072
    Name     : PING.EXE
    Path     : C:\Windows\System32\PING.EXE
```

---

### 4. Track process termination

When Windows reports:

```text
EXIT_PROCESS_DEBUG_EVENT
```

ShellTrace records:

* PID
* exit code
* end time
* execution duration
* success/failure status

Example:

```text
[PROCESS EXIT]
    PID       : 11312
    Name      : cmd.exe
    Exit code : 7
    Duration  : 16 ms
    Result    : FAILED
```

An exit code of `0` is treated as successful.

A non-zero exit code is treated as a failure.

---

### 5. Build the process hierarchy

ShellTrace stores parent-child relationships and reconstructs the resulting process tree.

Example:

```text
cmd.exe [PID=8072, exit=0]
    PING.EXE [PID=8416, exit=0]
    powershell.exe [PID=5040, exit=0]
    cmd.exe [PID=9060, exit=0]
    cmd.exe [PID=11312, exit=7]
```

This makes it possible to see which processes were created by the target workload.

> **Note on PPID accuracy:** ShellTrace reports the PPID recorded via the process's inherited parent relationship at creation time. This is sufficient for the batch, PowerShell, and executable workloads it has been tested against, but on Windows the reported parent is not guaranteed in every scenario to be the exact historical creator of a process (for example, in cases involving parent-process spoofing, handle inheritance tricks, or certain service/shim launch paths). Treat PPID as a reliable indicator for typical script and tool execution, not as a tamper-proof audit trail.

---

## Supported Targets

### Batch files

```cmd
shelltrace.exe test.bat
```

or:

```cmd
shelltrace.exe test.cmd
```

ShellTrace launches these through:

```text
cmd.exe /d /c
```

---

### PowerShell scripts

```cmd
shelltrace.exe test.ps1
```

ShellTrace launches PowerShell using:

```text
powershell.exe -NoProfile -ExecutionPolicy Bypass -File
```

---

### Executables

ShellTrace can also trace an executable directly:

```cmd
shelltrace.exe program.exe
```

---

## Example Test

Create a file called:

```text
test.bat
```

with:

```bat
@echo off

echo ==========================
echo ShellTrace Test
echo ==========================

echo.
echo [1] Starting ping...
ping 127.0.0.1 -n 3 > nul

echo.
echo [2] Starting PowerShell...
powershell.exe -NoProfile -Command "Write-Host PowerShell child process"

echo.
echo [3] Starting another command...
cmd.exe /c "echo Nested CMD process"

echo.
echo [4] Intentional failure...
cmd.exe /c "exit /b 7"

echo.
echo [5] Test finished.

exit /b 0
```

Run:

```cmd
shelltrace.exe test.bat
```

---

## Demo

The output below is from an actual run of `shelltrace.exe test.bat` on the test machine described in [Development Status](#development-status). It is reproduced here verbatim (also saved at `docs/demo.txt`) so the tool's behavior is verifiable rather than illustrative only.

```text
================================================
                 SHELLTRACE
        Windows Process Execution Tracer
================================================

Target:
  cmd.exe /d /c "test.bat"

Root PID:
  8072

Tracing...

[PROCESS START]
    PID      : 8072
    PPID     : 0
    Name     : cmd.exe
    Path     : C:\Windows\System32\cmd.exe

[PROCESS START]
    PID      : 8416
    PPID     : 8072
    Name     : PING.EXE
    Path     : C:\Windows\System32\PING.EXE

[PROCESS EXIT]
    PID       : 8416
    Name      : PING.EXE
    Exit code : 0
    Duration  : 2031 ms
    Result    : SUCCESS

[PROCESS START]
    PID      : 5040
    PPID     : 8072
    Name     : powershell.exe
    Path     : C:\Windows\System32\WindowsPowerShell\v1.0\powershell.exe

PowerShell child process

[PROCESS EXIT]
    PID       : 5040
    Name      : powershell.exe
    Exit code : 0
    Duration  : 547 ms
    Result    : SUCCESS

[PROCESS START]
    PID      : 9060
    PPID     : 8072
    Name      : cmd.exe
    Path     : C:\Windows\System32\cmd.exe

Nested CMD process

[PROCESS EXIT]
    PID       : 9060
    Name      : cmd.exe
    Exit code : 0
    Duration  : 31 ms
    Result    : SUCCESS

[PROCESS START]
    PID      : 11312
    PPID     : 8072
    Name      : cmd.exe
    Path     : C:\Windows\System32\cmd.exe

[PROCESS EXIT]
    PID       : 11312
    Name      : cmd.exe
    Exit code : 7
    Duration  : 16 ms
    Result    : FAILED

[PROCESS EXIT]
    PID       : 8072
    Name      : cmd.exe
    Exit code : 0
    Duration  : 2828 ms
    Result    : SUCCESS
```

### Trace Summary

After execution, ShellTrace produces a summary:

```text
================================================
                  TRACE SUMMARY
================================================

Processes observed : 5
Successful         : 4
Failed              : 1

Process tree:

cmd.exe [PID=8072, exit=0]
    PING.EXE [PID=8416, exit=0]
    powershell.exe [PID=5040, exit=0]
    cmd.exe [PID=9060, exit=0]
    cmd.exe [PID=11312, exit=7]

================================================
```

---

## Implementation Details

### Language

```text
C
```

### Platform

```text
Windows
```

### APIs

ShellTrace currently uses Windows APIs including:

```c
CreateProcessA()
WaitForDebugEvent()
ContinueDebugEvent()
QueryFullProcessImageNameA()
OpenProcess()
GetProcAddress()
```

It dynamically accesses:

```text
NtQueryInformationProcess
```

to obtain parent-process information without relying on the older `tlhelp32.h` process enumeration interface.

---

## Why Use the Windows Debug API?

A simple implementation could repeatedly inspect the system:

```text
scan processes
    |
wait
    |
scan processes
    |
wait
    |
scan processes
```

That approach is essentially polling.

ShellTrace instead uses an event-driven model:

```text
Windows
   |
   | process created
   v
ShellTrace receives event
   |
   | process exited
   v
ShellTrace receives event
```

This makes the tracing mechanism directly connected to Windows' debugging infrastructure.

---

## Project Structure

Recommended repository layout:

```text
ShellTrace/
│
├── src/
│   └── shelltrace.c
│
├── tests/
│   ├── test.bat
│   └── test.ps1
│
├── docs/
│   └── demo.txt
│
├── README.md
├── LICENSE
└── .gitignore
```

---

## Building

The project can be compiled with a Windows C compiler such as MinGW/TDM-GCC.

The original implementation was tested using:

```text
Dev-C++ 5.11
TDM-GCC 4.9.2 64-bit
Windows 10
```

Compile:

```text
shelltrace.c
```

to produce:

```text
shelltrace.exe
```

---

## Running

From Command Prompt:

```cmd
cd C:\path\to\ShellTrace
```

Then:

```cmd
shelltrace.exe test.bat
```

PowerShell:

```cmd
shelltrace.exe test.ps1
```

Executable:

```cmd
shelltrace.exe program.exe
```

---

## Current Limitations

This is currently an early version of ShellTrace.

### 1. First-chance exceptions can be noisy

Windows can report first-chance exceptions that are handled internally by applications.

For example, PowerShell may generate several exception events even though it eventually exits successfully:

```text
0x80000003
0xE0434352
0xE06D7363
```

These do **not necessarily indicate application failure**.

Improved exception filtering is planned.

---

### 2. Command-line arguments are not currently captured for every child

The current implementation records the executable path:

```text
C:\Windows\System32\powershell.exe
```

but does not yet reconstruct the complete command line of every child process.

---

### 3. Thread-level tracing is limited

ShellTrace receives thread events from Windows but currently focuses on process-level tracing rather than maintaining a complete thread database.

---

### 4. DLL events are not currently displayed

Windows provides DLL load/unload events, but ShellTrace currently ignores them.

---

### 5. PPID reflects the inherited parent relationship, not a guaranteed audit trail

As noted above, the recorded PPID is accurate for the tested workloads but should not be treated as an unspoofable record of the true process creator in every possible Windows scenario.

---

### 6. Output is currently human-readable only

The current implementation prints results directly to the terminal.

Structured formats such as:

```text
JSON
CSV
```

can be added later.

---

## Roadmap

### v0.1 — Current

* [x] Windows-native implementation
* [x] Process creation tracing
* [x] Process termination tracing
* [x] PID tracking
* [x] PPID tracking
* [x] Executable path detection
* [x] Exit-code detection
* [x] Execution timing
* [x] Process tree
* [x] BAT/CMD support
* [x] PowerShell support
* [x] EXE support
* [x] Basic exception detection

### Planned

* [ ] Filter handled first-chance exceptions
* [ ] Capture complete child command lines
* [ ] Improve failure classification
* [ ] JSON output
* [ ] CSV output
* [ ] Thread tracking
* [ ] DLL-load tracing
* [ ] Better process-tree visualization
* [ ] Configurable output levels
* [ ] Trace filtering by process name/PID
* [ ] Automated failure summaries

---

## Example Use Cases

### Debugging build scripts

Instead of only seeing:

```text
Build failed.
```

ShellTrace can expose:

```text
build.cmd
    |
    +-- compiler.exe → exit 0
    |
    +-- linker.exe → exit 0
    |
    +-- tool.exe → exit 1
```

---

### Debugging deployment scripts

A deployment script may launch several utilities and PowerShell commands.

ShellTrace can show:

```text
deploy.cmd
    |
    +-- powershell.exe
    +-- curl.exe
    +-- service-tool.exe
    +-- cmd.exe
```

along with their process IDs, exit codes, and durations.

---

### Understanding process creation

ShellTrace can also be used as a small systems-programming experiment for understanding:

* Windows process creation
* Parent-child process relationships
* Process termination
* Exit codes
* Debugger event loops
* Windows process execution
* Basic exception handling

---

## Technical Concepts Demonstrated

This project demonstrates practical knowledge of:

```text
C
│
├── Structures
├── Pointers
├── Function pointers
├── String handling
├── Dynamic API lookup
└── Process state management

Windows Internals
│
├── Process creation
├── Process IDs
├── Parent-child relationships
├── Process termination
├── Exit codes
├── Debug events
└── Exception events

Systems Programming
│
├── Event-driven tracing
├── Process lifecycle observation
├── Execution timing
└── Process-tree reconstruction
```

---

## What ShellTrace Is Not

ShellTrace is **not** a replacement for:

* WinDbg
* Process Monitor
* Process Explorer
* Windows Performance Recorder
* a full debugger

Its purpose is narrower:

> **Trace the process lifecycle of a script or executable and present the resulting process execution tree in a simple, readable form.**

---

## Development Status

**Current version: `v0.1`**

ShellTrace has been successfully tested on Windows with batch workloads that launch:

* `cmd.exe`
* `PING.EXE`
* `powershell.exe`

The current implementation correctly detects process creation, process termination, exit codes, execution duration, parent-child relationships, and process hierarchy.

The next major improvement is reducing noise from first-chance exceptions and improving failure reporting.

---

## License

This project is released under the MIT License.

See [`LICENSE`](LICENSE) for details.
