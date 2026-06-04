#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "windows_top/windows_top.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <conio.h>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const wchar_t* kVersion = L"0.1.0";
constexpr double kDefaultDelaySeconds = 3.0;
constexpr int kDefaultIterations = -1;

struct Options {
  bool batch_mode = false;
  int iterations = kDefaultIterations;
  double delay_seconds = kDefaultDelaySeconds;
  DWORD pid = 0;
  std::wstring sort_by = L"CPU";
  bool no_headers = false;
};

struct ProcessSample {
  DWORD pid = 0;
  DWORD ppid = 0;
  DWORD threads = 0;
  LONG base_priority = 0;
  std::wstring name;
  SIZE_T working_set = 0;
  SIZE_T private_bytes = 0;
  unsigned long long cpu_time = 0;
};

struct ProcessRow {
  ProcessSample sample;
  double cpu_percent = 0.0;
  double mem_percent = 0.0;
};

struct SystemSnapshot {
  unsigned long long idle = 0;
  unsigned long long kernel = 0;
  unsigned long long user = 0;
  unsigned long long total_memory = 0;
  unsigned long long available_memory = 0;
  unsigned long long uptime_ms = 0;
};

unsigned long long filetime_to_u64(const FILETIME& ft) {
  return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) |
         static_cast<unsigned long long>(ft.dwLowDateTime);
}

std::string wide_to_utf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }

  const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                         static_cast<int>(value.size()), nullptr,
                                         0, nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }

  std::string out(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                      out.data(), needed, nullptr, nullptr);
  return out;
}

std::wstring upper(std::wstring value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(towupper(ch)); });
  return value;
}

double parse_double(const wchar_t* value, double fallback) {
  if (value == nullptr) {
    return fallback;
  }

  wchar_t* end = nullptr;
  const double parsed = wcstod(value, &end);
  if (end == value || parsed <= 0.0 || !std::isfinite(parsed)) {
    return fallback;
  }

  return parsed;
}

int parse_int(const wchar_t* value, int fallback) {
  if (value == nullptr) {
    return fallback;
  }

  wchar_t* end = nullptr;
  const long parsed = wcstol(value, &end, 10);
  if (end == value || parsed == 0 || parsed < -1) {
    return fallback;
  }

  return static_cast<int>(parsed);
}

DWORD parse_pid(const wchar_t* value) {
  if (value == nullptr) {
    return 0;
  }

  wchar_t* end = nullptr;
  const unsigned long parsed = wcstoul(value, &end, 10);
  if (end == value) {
    return 0;
  }

  return static_cast<DWORD>(parsed);
}

void print_usage() {
  std::cout
      << "Usage: top [OPTIONS]\n\n"
      << "Display dynamic real-time information about Windows processes.\n\n"
      << "Options:\n"
      << "  -b, --batch           print snapshots instead of interactive refresh\n"
      << "  -n, --iterations N    number of updates before exiting\n"
      << "  -d, --delay SECONDS   delay between updates\n"
      << "  -o, --sort FIELD      sort by CPU, MEM, TIME, PID, or NAME\n"
      << "  -p, --pid PID         show only one process\n"
      << "      --no-headers      omit summary and table headers\n"
      << "  -h, --help            show this help\n"
      << "  -v, --version         show version\n";
}

void print_version() {
  std::cout << "top (Windows Top) " << wide_to_utf8(kVersion) << "\n";
}

bool parse_args(int argc, const wchar_t* const* argv, Options& options,
                bool& handled) {
  handled = false;

  for (int i = 1; i < argc; ++i) {
    const std::wstring arg = argv[i] ? argv[i] : L"";

    if (arg == L"-h" || arg == L"--help") {
      print_usage();
      handled = true;
      return true;
    }
    if (arg == L"-v" || arg == L"--version") {
      print_version();
      handled = true;
      return true;
    }
    if (arg == L"-b" || arg == L"--batch") {
      options.batch_mode = true;
      continue;
    }
    if (arg == L"--no-headers") {
      options.no_headers = true;
      continue;
    }
    if ((arg == L"-n" || arg == L"--iterations") && i + 1 < argc) {
      options.iterations = parse_int(argv[++i], options.iterations);
      continue;
    }
    if ((arg == L"-d" || arg == L"--delay") && i + 1 < argc) {
      options.delay_seconds = parse_double(argv[++i], options.delay_seconds);
      continue;
    }
    if ((arg == L"-o" || arg == L"--sort") && i + 1 < argc) {
      options.sort_by = upper(argv[++i]);
      continue;
    }
    if ((arg == L"-p" || arg == L"--pid") && i + 1 < argc) {
      options.pid = parse_pid(argv[++i]);
      continue;
    }

    std::wcerr << L"top: unknown option: " << arg << L"\n";
    return false;
  }

  return true;
}

SystemSnapshot read_system_snapshot() {
  SystemSnapshot snapshot;

  FILETIME idle{}, kernel{}, user{};
  if (GetSystemTimes(&idle, &kernel, &user)) {
    snapshot.idle = filetime_to_u64(idle);
    snapshot.kernel = filetime_to_u64(kernel);
    snapshot.user = filetime_to_u64(user);
  }

  MEMORYSTATUSEX memory{};
  memory.dwLength = sizeof(memory);
  if (GlobalMemoryStatusEx(&memory)) {
    snapshot.total_memory = memory.ullTotalPhys;
    snapshot.available_memory = memory.ullAvailPhys;
  }

  snapshot.uptime_ms = GetTickCount64();
  return snapshot;
}

std::wstring process_name_from_entry(const PROCESSENTRY32W& entry) {
  return entry.szExeFile[0] ? std::wstring(entry.szExeFile)
                            : std::wstring(L"[unknown]");
}

std::vector<ProcessSample> enumerate_processes() {
  std::vector<ProcessSample> processes;

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return processes;
  }

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);

  if (!Process32FirstW(snapshot, &entry)) {
    CloseHandle(snapshot);
    return processes;
  }

  do {
    ProcessSample sample;
    sample.pid = entry.th32ProcessID;
    sample.ppid = entry.th32ParentProcessID;
    sample.threads = entry.cntThreads;
    sample.base_priority = entry.pcPriClassBase;
    sample.name = process_name_from_entry(entry);

    HANDLE process =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE,
                    entry.th32ProcessID);
    if (process != nullptr) {
      FILETIME create{}, exit{}, kernel{}, user{};
      if (GetProcessTimes(process, &create, &exit, &kernel, &user)) {
        sample.cpu_time = filetime_to_u64(kernel) + filetime_to_u64(user);
      }

      PROCESS_MEMORY_COUNTERS_EX memory{};
      if (GetProcessMemoryInfo(process,
                               reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(
                                   &memory),
                               sizeof(memory))) {
        sample.working_set = memory.WorkingSetSize;
        sample.private_bytes = memory.PrivateUsage;
      }

      CloseHandle(process);
    }

    processes.push_back(std::move(sample));
  } while (Process32NextW(snapshot, &entry));

  CloseHandle(snapshot);
  return processes;
}

std::map<DWORD, ProcessSample> by_pid(const std::vector<ProcessSample>& samples) {
  std::map<DWORD, ProcessSample> out;
  for (const auto& sample : samples) {
    out[sample.pid] = sample;
  }
  return out;
}

std::vector<ProcessRow> build_rows(const std::vector<ProcessSample>& previous,
                                   const std::vector<ProcessSample>& current,
                                   const SystemSnapshot& previous_system,
                                   const SystemSnapshot& current_system,
                                   const Options& options) {
  const auto previous_by_pid = by_pid(previous);
  const unsigned long long previous_total =
      previous_system.kernel + previous_system.user;
  const unsigned long long current_total =
      current_system.kernel + current_system.user;
  const unsigned long long system_delta =
      current_total > previous_total ? current_total - previous_total : 1;

  std::vector<ProcessRow> rows;
  rows.reserve(current.size());

  for (const auto& sample : current) {
    if (options.pid != 0 && sample.pid != options.pid) {
      continue;
    }

    ProcessRow row;
    row.sample = sample;

    const auto it = previous_by_pid.find(sample.pid);
    if (it != previous_by_pid.end() && sample.cpu_time >= it->second.cpu_time) {
      const unsigned long long process_delta =
          sample.cpu_time - it->second.cpu_time;
      row.cpu_percent =
          static_cast<double>(process_delta) * 100.0 /
          static_cast<double>(system_delta);
    }

    if (current_system.total_memory != 0) {
      row.mem_percent = static_cast<double>(sample.working_set) * 100.0 /
                        static_cast<double>(current_system.total_memory);
    }

    rows.push_back(std::move(row));
  }

  return rows;
}

void sort_rows(std::vector<ProcessRow>& rows, const std::wstring& sort_by) {
  const std::wstring key = upper(sort_by);
  std::sort(rows.begin(), rows.end(), [&](const ProcessRow& a,
                                          const ProcessRow& b) {
    if (key == L"MEM") {
      return a.sample.working_set > b.sample.working_set;
    }
    if (key == L"TIME") {
      return a.sample.cpu_time > b.sample.cpu_time;
    }
    if (key == L"PID") {
      return a.sample.pid < b.sample.pid;
    }
    if (key == L"NAME" || key == L"COMMAND") {
      return _wcsicmp(a.sample.name.c_str(), b.sample.name.c_str()) < 0;
    }
    return a.cpu_percent > b.cpu_percent;
  });
}

std::string format_memory(unsigned long long bytes) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(1)
      << static_cast<double>(bytes) / 1024.0 / 1024.0;
  return out.str();
}

std::string format_cpu_time(unsigned long long filetime_ticks) {
  const unsigned long long total_seconds = filetime_ticks / 10000000ULL;
  const unsigned long long minutes = total_seconds / 60ULL;
  const unsigned long long seconds = total_seconds % 60ULL;

  std::ostringstream out;
  out << minutes << ":" << std::setw(2) << std::setfill('0') << seconds;
  return out.str();
}

void print_summary(const SystemSnapshot& system, size_t process_count,
                   double cpu_usage) {
  const unsigned long long used_memory =
      system.total_memory > system.available_memory
          ? system.total_memory - system.available_memory
          : 0;
  const unsigned long long uptime_seconds = system.uptime_ms / 1000ULL;
  const unsigned long long uptime_hours = uptime_seconds / 3600ULL;
  const unsigned long long uptime_minutes = (uptime_seconds / 60ULL) % 60ULL;

  SYSTEMTIME local_time{};
  GetLocalTime(&local_time);

  std::cout << "top - " << std::setw(2) << std::setfill('0')
            << local_time.wHour << ":" << std::setw(2) << local_time.wMinute
            << ":" << std::setw(2) << local_time.wSecond << std::setfill(' ')
            << " up " << uptime_hours << ":" << std::setw(2)
            << std::setfill('0') << uptime_minutes << std::setfill(' ')
            << ", " << process_count << " processes\n";
  std::cout << "%Cpu(s): " << std::fixed << std::setprecision(1) << cpu_usage
            << " used\n";
  std::cout << "MiB Mem : " << format_memory(system.total_memory) << " total, "
            << format_memory(system.available_memory) << " free, "
            << format_memory(used_memory) << " used\n";
}

void print_table(const std::vector<ProcessRow>& rows, const Options& options) {
  if (!options.no_headers) {
    std::cout << std::right << std::setw(7) << "PID" << " " << std::setw(7)
              << "PPID" << " " << std::setw(4) << "THR" << " "
              << std::setw(4) << "PRI" << " " << std::setw(6) << "%CPU"
              << " " << std::setw(6) << "%MEM" << " " << std::setw(8)
              << "RES" << " " << std::setw(8) << "TIME" << " COMMAND\n";
  }

  for (const auto& row : rows) {
    std::cout << std::right << std::setw(7) << row.sample.pid << " "
              << std::setw(7) << row.sample.ppid << " " << std::setw(4)
              << row.sample.threads << " " << std::setw(4)
              << row.sample.base_priority << " " << std::setw(6)
              << std::fixed << std::setprecision(1) << row.cpu_percent << " "
              << std::setw(6) << std::fixed << std::setprecision(1)
              << row.mem_percent << " " << std::setw(8)
              << format_memory(row.sample.working_set) << " " << std::setw(8)
              << format_cpu_time(row.sample.cpu_time) << " "
              << wide_to_utf8(row.sample.name) << "\n";
  }
}

double calculate_system_cpu(const SystemSnapshot& previous,
                            const SystemSnapshot& current) {
  const unsigned long long previous_total = previous.kernel + previous.user;
  const unsigned long long current_total = current.kernel + current.user;
  const unsigned long long previous_idle = previous.idle;
  const unsigned long long current_idle = current.idle;

  if (current_total <= previous_total) {
    return 0.0;
  }

  const unsigned long long total_delta = current_total - previous_total;
  const unsigned long long idle_delta =
      current_idle > previous_idle ? current_idle - previous_idle : 0;

  return (1.0 - static_cast<double>(idle_delta) /
                    static_cast<double>(total_delta)) *
         100.0;
}

void enable_virtual_terminal() {
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out == INVALID_HANDLE_VALUE) {
    return;
  }

  DWORD mode = 0;
  if (GetConsoleMode(out, &mode)) {
    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
  }
}

int run_top(const Options& options) {
  SetConsoleOutputCP(CP_UTF8);
  enable_virtual_terminal();

  Options effective = options;
  if (effective.batch_mode && effective.iterations == kDefaultIterations) {
    effective.iterations = 1;
  }

  auto previous_system = read_system_snapshot();
  auto previous_processes = enumerate_processes();

  const auto delay = std::chrono::duration<double>(effective.delay_seconds);
  int iteration = 0;

  while (effective.iterations < 0 || iteration < effective.iterations) {
    std::this_thread::sleep_for(delay);

    const auto current_system = read_system_snapshot();
    const auto current_processes = enumerate_processes();
    auto rows = build_rows(previous_processes, current_processes,
                           previous_system, current_system, effective);
    sort_rows(rows, effective.sort_by);

    if (!effective.batch_mode) {
      std::cout << "\x1b[2J\x1b[H";
    }

    const double cpu_usage = calculate_system_cpu(previous_system, current_system);
    if (!effective.no_headers) {
      print_summary(current_system, current_processes.size(), cpu_usage);
    }
    print_table(rows, effective);

    previous_system = current_system;
    previous_processes = current_processes;
    ++iteration;

    if (!effective.batch_mode && _kbhit()) {
      const int ch = _getch();
      if (ch == 'q' || ch == 'Q') {
        break;
      }
    }
  }

  return 0;
}

Options from_c_options(const windows_top_options* options) {
  Options out;
  if (options == nullptr) {
    out.batch_mode = true;
    out.iterations = 1;
    out.delay_seconds = 0.1;
    return out;
  }

  out.batch_mode = options->batch_mode != 0;
  out.iterations = options->iterations == 0 ? kDefaultIterations
                                            : options->iterations;
  out.delay_seconds = options->delay_seconds > 0.0 ? options->delay_seconds
                                                   : kDefaultDelaySeconds;
  out.pid = static_cast<DWORD>(options->pid);
  out.sort_by = options->sort_by ? upper(options->sort_by) : L"CPU";
  out.no_headers = options->no_headers != 0;
  return out;
}

}  // namespace

const wchar_t* windows_top_version(void) { return kVersion; }

int windows_top_run(int argc, const wchar_t* const* argv) {
  Options options;
  bool handled = false;
  if (!parse_args(argc, argv, options, handled)) {
    return 1;
  }
  if (handled) {
    return 0;
  }

  return run_top(options);
}

int windows_top_run_with_options(const windows_top_options* options) {
  return run_top(from_c_options(options));
}

