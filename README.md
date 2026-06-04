# Windows Top

A native Windows implementation of the Unix `top` command, extracted from the WinuxCmd project and rebuilt as a standalone C++17 tool.

Windows Top provides a small `top.exe` for process monitoring on Windows without requiring WSL, MSYS2, Sysinternals, or PowerShell-specific commands.

## Status

This project is early. The first goal is a reliable batch mode and a simple interactive refresh loop. More terminal UI features can be added later.

## Build

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

## Usage

```powershell
top
top -b -n 1
top -b -n 3 -d 1
top -c
top -H
top -o CPU
top -o MEM
top -p 1234,5678
top -u SYSTEM
```

In interactive mode, press `q` or `Esc` to exit. Press `P`, `M`, `T`, or `N`
to sort by CPU, memory, time, or PID. Press `C`, `H`, or `I` to toggle command
paths, thread counts, or idle processes. Press `S`/`D` to change delay, `K` to
terminate a process, `R` to change process priority, or `U` to set a user
filter.

Options:

```text
-b, --batch          print snapshots instead of interactive screen refresh
-c, --command        show process path when available
-n, --iterations N   number of updates before exiting
-d, --delay SECONDS  delay between updates
-H, --threads        show thread count column
-i, --idle-toggle    hide processes with 0% CPU in this snapshot
-o, --sort FIELD     sort by CPU, MEM, TIME, PID, or NAME
-p, --pid PID[,PID]  show only selected processes
-u, --user USER      show only matching process owners
-U, --User USER      alias for --user on Windows
-w, --width WIDTH    limit command column width
--no-headers         omit summary and table headers
-h, --help           show help
-v, --version        show version
```

## C ABI

The project exposes a small C ABI so other projects can embed it without depending on C++ symbols:

```c
#include <winuxcmd/windows_top.h>

int exit_code = windows_top_run(argc, argv);
```

For structured embedding:

```c
windows_top_options options = {0};
options.batch_mode = 1;
options.iterations = 1;
options.delay_seconds = 0.2;
options.sort_by = L"CPU";
options.show_threads = 1;
options.width = 120;

int exit_code = windows_top_run_with_options(&options);
```

## Relationship To WinuxCmd

This project is based on ideas and Windows process-monitoring code from WinuxCmd, but it is intentionally standalone:

- no WinuxCmd pipeline dependency
- no C++20 modules
- no command macro system
- C++17 and CMake only
