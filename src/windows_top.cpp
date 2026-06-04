/*
 * Copyright (c) 2026 caomengxuan666
 *
 * This file is part of windows-top.
 *
 * windows-top is a standalone C++17 implementation of a native Windows
 * top command, extracted from the WinuxCmd project.
 *
 * Licensed under the MIT License. See the LICENSE file in the project root
 * for the full license text.
 */

#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include "winuxcmd/windows_top.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <conio.h>
#include <cwctype>
#include <iomanip>
#include <iostream>
#include <map>
#include <set>
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
  std::set<DWORD> pids;
  std::wstring sort_by = L"CPU";
  std::wstring user_filter;
  bool no_headers = false;
  bool show_command = false;
  bool show_threads = false;
  bool ignore_idle = false;
  int width = 120;
};

struct ProcessSample {
  DWORD pid = 0;
  DWORD ppid = 0;
  DWORD threads = 0;
  LONG base_priority = 0;
  std::wstring name;
  std::wstring path;
  std::string user;
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

std::set<DWORD> parse_pid_list(const wchar_t* value) {
  std::set<DWORD> pids;
  if (value == nullptr) {
    return pids;
  }

  std::wstring text(value);
  size_t start = 0;
  while (start <= text.size()) {
    const size_t comma = text.find(L',', start);
    const size_t end = comma == std::wstring::npos ? text.size() : comma;
    const std::wstring part = text.substr(start, end - start);
    if (!part.empty()) {
      const DWORD pid = parse_pid(part.c_str());
      if (pid != 0) {
        pids.insert(pid);
      }
    }
    if (comma == std::wstring::npos) {
      break;
    }
    start = comma + 1;
  }

  return pids;
}

void add_pid_list(std::set<DWORD>& target, const wchar_t* value) {
  auto parsed = parse_pid_list(value);
  target.insert(parsed.begin(), parsed.end());
}

void print_usage() {
  std::cout
      << "Usage: top [OPTIONS]\n\n"
      << "Display dynamic real-time information about Windows processes.\n\n"
      << "Options:\n"
      << "  -b, --batch           print snapshots instead of interactive refresh\n"
      << "  -c, --command         show process path when available\n"
      << "  -n, --iterations N    number of updates before exiting\n"
      << "  -d, --delay SECONDS   delay between updates\n"
      << "  -H, --threads         show thread count column\n"
      << "  -i, --idle-toggle     hide processes with 0% CPU in this snapshot\n"
      << "  -o, --sort FIELD      sort by CPU, MEM, TIME, PID, or NAME\n"
      << "  -p, --pid PID[,PID]   show only selected processes\n"
      << "  -u, --user USER       show only matching process owners\n"
      << "  -U, --User USER       alias for --user on Windows\n"
      << "  -w, --width WIDTH     limit command column width\n"
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
    if (arg == L"-c" || arg == L"--command") {
      options.show_command = true;
      continue;
    }
    if (arg == L"-H" || arg == L"--threads") {
      options.show_threads = true;
      continue;
    }
    if (arg == L"-i" || arg == L"--idle-toggle") {
      options.ignore_idle = true;
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
      add_pid_list(options.pids, argv[++i]);
      continue;
    }
    if ((arg == L"-u" || arg == L"--user" || arg == L"-U" ||
         arg == L"--User") &&
        i + 1 < argc) {
      options.user_filter = upper(argv[++i]);
      continue;
    }
    if ((arg == L"-w" || arg == L"--width") && i + 1 < argc) {
      options.width = parse_int(argv[++i], options.width);
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

std::string get_process_user(HANDLE process) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
    return "N/A";
  }

  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  if (size == 0) {
    CloseHandle(token);
    return "N/A";
  }

  std::vector<unsigned char> buffer(size);
  if (!GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
    CloseHandle(token);
    return "N/A";
  }

  const auto* token_user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
  WCHAR user[256]{};
  WCHAR domain[256]{};
  DWORD user_size = static_cast<DWORD>(sizeof(user) / sizeof(user[0]));
  DWORD domain_size = static_cast<DWORD>(sizeof(domain) / sizeof(domain[0]));
  SID_NAME_USE use = SidTypeUnknown;

  if (!LookupAccountSidW(nullptr, token_user->User.Sid, user, &user_size, domain,
                         &domain_size, &use)) {
    CloseHandle(token);
    return "N/A";
  }

  CloseHandle(token);

  if (domain[0] != 0) {
    return wide_to_utf8(std::wstring(domain) + L"\\" + user);
  }
  return wide_to_utf8(user);
}

std::wstring get_process_path(HANDLE process) {
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
    return {};
  }

  path.resize(size);
  return path;
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
      sample.user = get_process_user(process);
      sample.path = get_process_path(process);

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
    if (!options.pids.empty() && options.pids.count(sample.pid) == 0) {
      continue;
    }
    if (!options.user_filter.empty()) {
      std::wstring user_wide(sample.user.begin(), sample.user.end());
      if (upper(user_wide).find(options.user_filter) == std::wstring::npos) {
        continue;
      }
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

    if (options.ignore_idle && row.cpu_percent == 0.0) {
      continue;
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

struct ConsoleView {
  short left = 0;
  short top = 0;
  short width = 80;
  short height = 25;
  WORD attributes = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
  bool valid = false;
};

bool is_console_handle(HANDLE handle);

class ConsoleAttributeGuard {
 public:
  explicit ConsoleAttributeGuard(HANDLE out) : out_(out) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (is_console_handle(out_) && GetConsoleScreenBufferInfo(out_, &info)) {
      original_ = info.wAttributes;
      active_ = true;
    }
  }

  ConsoleAttributeGuard(const ConsoleAttributeGuard&) = delete;
  ConsoleAttributeGuard& operator=(const ConsoleAttributeGuard&) = delete;

  ~ConsoleAttributeGuard() { restore(); }

  void restore() {
    if (active_) {
      SetConsoleTextAttribute(out_, original_);
    }
  }

 private:
  HANDLE out_ = nullptr;
  WORD original_ = 0;
  bool active_ = false;
};

ConsoleView console_view(HANDLE out);
short console_width(HANDLE out);
std::string fit_line_to_width(std::string line, short width, bool pad);
void print_colored(HANDLE out, WORD color, const std::string& text,
                   bool interactive);
void print_header_bar(const std::string& text, short width, HANDLE out,
                      bool interactive);
bool handle_interactive_key(int ch, Options& options, bool& quit);

void print_table(const std::vector<ProcessRow>& rows, const Options& options,
                 HANDLE out, bool interactive, int max_rows = -1) {
  const short width = interactive ? console_width(out) : 0;
  if (!options.no_headers) {
    std::ostringstream header;
    header << std::right << std::setw(7) << "PID" << " " << std::setw(7)
           << "PPID" << " ";
    if (options.show_threads) {
      header << std::setw(4) << "THR" << " ";
    }
    header << std::setw(4) << "PRI" << " " << std::setw(6) << "%CPU" << " "
           << std::setw(6) << "%MEM" << " " << std::setw(8) << "RES" << " "
           << std::setw(8) << "TIME" << " COMMAND";

    if (interactive) {
      print_header_bar(header.str(), width, out, true);
    } else {
      std::cout << header.str() << "\n";
    }
  }

  int printed = 0;
  for (const auto& row : rows) {
    if (max_rows >= 0 && printed >= max_rows) {
      break;
    }

    std::string command =
        options.show_command && !row.sample.path.empty()
            ? wide_to_utf8(row.sample.path)
            : wide_to_utf8(row.sample.name);
    if (options.width > 0 && static_cast<int>(command.size()) > options.width) {
      command.resize(static_cast<size_t>(options.width));
    }

    std::ostringstream line;
    line << std::right << std::setw(7) << row.sample.pid << " "
         << std::setw(7) << row.sample.ppid << " ";
    if (options.show_threads) {
      line << std::setw(4) << row.sample.threads << " ";
    }
    line << std::setw(4) << row.sample.base_priority << " " << std::setw(6)
         << std::fixed << std::setprecision(1) << row.cpu_percent << " "
         << std::setw(6) << std::fixed << std::setprecision(1)
         << row.mem_percent << " " << std::setw(8)
         << format_memory(row.sample.working_set) << " " << std::setw(8)
         << format_cpu_time(row.sample.cpu_time) << " " << command;

    std::string output = line.str();
    if (interactive) {
      output = fit_line_to_width(std::move(output), width, false);
    }
    std::cout << output << "\n";
    ++printed;
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

bool is_console_handle(HANDLE handle) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE ||
      GetFileType(handle) != FILE_TYPE_CHAR) {
    return false;
  }

  DWORD mode = 0;
  return GetConsoleMode(handle, &mode) != 0;
}

ConsoleView console_view(HANDLE out) {
  ConsoleView view;
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (!GetConsoleScreenBufferInfo(out, &info)) {
    return view;
  }

  view.left = info.srWindow.Left;
  view.top = info.srWindow.Top;
  view.width = static_cast<short>(info.srWindow.Right - info.srWindow.Left + 1);
  view.height =
      static_cast<short>(info.srWindow.Bottom - info.srWindow.Top + 1);
  view.attributes = info.wAttributes;
  view.valid = true;
  return view;
}

short console_width(HANDLE out) { return console_view(out).width; }

std::string fit_line_to_width(std::string line, short width, bool pad) {
  if (width <= 0) {
    return line;
  }

  const size_t target = static_cast<size_t>(width);
  if (line.size() > target) {
    line.resize(target);
  } else if (pad && line.size() < target) {
    line.append(target - line.size(), ' ');
  }
  return line;
}

void print_colored(HANDLE out, WORD color, const std::string& text,
                   bool interactive) {
  if (!interactive || !is_console_handle(out)) {
    std::cout << text;
    return;
  }

  ConsoleAttributeGuard guard(out);
  if (!SetConsoleTextAttribute(out, color)) {
    std::cout << text;
    return;
  }

  std::cout << text;
}

WORD reversed_attributes(WORD attributes) {
  constexpr WORD kForeground = FOREGROUND_RED | FOREGROUND_GREEN |
                               FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  constexpr WORD kBackground = BACKGROUND_RED | BACKGROUND_GREEN |
                               BACKGROUND_BLUE | BACKGROUND_INTENSITY;

  WORD reversed = static_cast<WORD>(
      (attributes & ~(kForeground | kBackground)) |
      ((attributes & kForeground) << 4) | ((attributes & kBackground) >> 4));

  const WORD foreground = static_cast<WORD>(reversed & kForeground);
  const WORD background =
      static_cast<WORD>((reversed & kBackground) >> 4);
  if (foreground == background) {
    reversed = static_cast<WORD>((reversed & ~(kForeground | kBackground)) |
                                 BACKGROUND_RED | BACKGROUND_GREEN |
                                 BACKGROUND_BLUE);
  }

  return reversed;
}

bool clear_interactive_screen(HANDLE out, const ConsoleView& view) {
  if (!view.valid) {
    std::cout << "\x1b[2J\x1b[H\x1b[3J";
    return false;
  }

  DWORD written = 0;
  const DWORD row_cells = static_cast<DWORD>(view.width);
  for (short row = 0; row < view.height; ++row) {
    const COORD row_start{view.left, static_cast<short>(view.top + row)};
    FillConsoleOutputCharacterW(out, L' ', row_cells, row_start, &written);
    FillConsoleOutputAttribute(out, view.attributes, row_cells, row_start,
                               &written);
  }

  const COORD origin{view.left, view.top};
  SetConsoleCursorPosition(out, origin);
  return true;
}

std::string make_summary_line(const SystemSnapshot& system, size_t process_count,
                              double cpu_usage) {
  const unsigned long long uptime_seconds = system.uptime_ms / 1000ULL;
  const unsigned long long uptime_hours = uptime_seconds / 3600ULL;
  const unsigned long long uptime_minutes = (uptime_seconds / 60ULL) % 60ULL;

  SYSTEMTIME local_time{};
  GetLocalTime(&local_time);

  std::ostringstream out;
  out << "top - " << std::setw(2) << std::setfill('0') << local_time.wHour
      << ":" << std::setw(2) << local_time.wMinute << ":" << std::setw(2)
      << local_time.wSecond << std::setfill(' ') << " up " << uptime_hours
      << ":" << std::setw(2) << std::setfill('0') << uptime_minutes
      << std::setfill(' ') << ", " << process_count << " processes";
  if (cpu_usage >= 0.0) {
    out << " | cpu " << std::fixed << std::setprecision(1) << cpu_usage << "%";
  }

  return out.str();
}

void print_header_bar(const std::string& text, short width, HANDLE out,
                      bool interactive) {
  if (width <= 0) {
    width = 80;
  }

  std::string line = fit_line_to_width(text, width, true);

  if (interactive && is_console_handle(out)) {
    ConsoleAttributeGuard guard(out);
    SetConsoleTextAttribute(out,
                            reversed_attributes(console_view(out).attributes));
    std::cout << line << "\n";
    std::cout.flush();
    return;
  }

  std::cout << line << "\n";
}

void print_summary(const SystemSnapshot& system, size_t process_count,
                   double cpu_usage, HANDLE out, bool interactive) {
  const unsigned long long used_memory =
      system.total_memory > system.available_memory
          ? system.total_memory - system.available_memory
          : 0;
  const short width = interactive ? console_width(out) : 0;
  const double cpu = cpu_usage < 0.0 ? 0.0 : cpu_usage;
  const double user_cpu = cpu * 0.6;
  const double system_cpu = cpu * 0.2;
  const double idle_cpu = 100.0 - cpu;

  const std::string summary = make_summary_line(system, process_count, cpu);
  if (summary.rfind("top - ", 0) == 0) {
    print_colored(out, FOREGROUND_GREEN | FOREGROUND_INTENSITY, "top - ",
                  interactive);
    std::cout << fit_line_to_width(summary.substr(6), width, false) << "\n";
  } else {
    std::cout << fit_line_to_width(summary, width, false) << "\n";
  }

  std::ostringstream tasks_line;
  tasks_line << "Tasks: " << std::setw(4) << process_count
             << " total,      1 running, " << std::setw(4)
             << (process_count > 0 ? process_count - 1 : 0)
             << " sleeping,   0 stopped,   0 zombie";
  std::cout << fit_line_to_width(tasks_line.str(), width, false) << "\n";

  std::ostringstream cpu_line;
  cpu_line << "%Cpu(s): " << std::fixed << std::setprecision(1)
           << std::setw(5) << user_cpu << " us, " << std::setw(5)
           << system_cpu << " sy, " << std::setw(5) << 0.0 << " ni, "
           << std::setw(5) << idle_cpu << " id, " << std::setw(5) << 0.0
           << " wa, " << std::setw(5) << 0.0 << " hi, " << std::setw(5)
           << 0.0 << " si, " << std::setw(5) << 0.0 << " st";
  std::cout << fit_line_to_width(cpu_line.str(), width, false) << "\n";

  std::ostringstream memory_line;
  memory_line << "MiB Mem : " << format_memory(system.total_memory)
              << " total, " << format_memory(system.available_memory)
              << " free, " << format_memory(used_memory) << " used";
  std::cout << fit_line_to_width(memory_line.str(), width, false) << "\n";

  std::ostringstream swap_line;
  swap_line << "MiB Swap:      0.0 total,      0.0 free,      0.0 used. "
            << format_memory(system.available_memory) << " avail Mem";
  std::cout << fit_line_to_width(swap_line.str(), width, false) << "\n\n";
}

bool handle_interactive_key(int ch, Options& options, bool& quit) {
  switch (ch) {
    case 'q':
    case 'Q':
    case 27:
      quit = true;
      return true;
    case 'p':
    case 'P':
      options.sort_by = L"CPU";
      return true;
    case 'm':
    case 'M':
      options.sort_by = L"MEM";
      return true;
    case 't':
    case 'T':
      options.sort_by = L"TIME";
      return true;
    case 'n':
    case 'N':
      options.sort_by = L"PID";
      return true;
    case 'c':
    case 'C':
      options.show_command = !options.show_command;
      return true;
    case 'h':
    case 'H':
      options.show_threads = !options.show_threads;
      return true;
    case 'i':
    case 'I':
      options.ignore_idle = !options.ignore_idle;
      return true;
    default:
      return false;
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
  HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  const bool interactive = !effective.batch_mode && is_console_handle(out);
  ConsoleAttributeGuard session_attributes(out);

  while (effective.iterations < 0 || iteration < effective.iterations) {
    SystemSnapshot current_system = previous_system;
    std::vector<ProcessSample> current_processes = previous_processes;

    if (iteration > 0) {
      if (effective.batch_mode) {
        std::this_thread::sleep_for(delay);
      } else {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start < delay) {
          if (_kbhit()) {
            const int ch = _getch();
            bool quit = false;
            if (handle_interactive_key(ch, effective, quit)) {
              if (quit) {
                return 0;
              }
              break;
            }
          }
          const auto remaining =
              delay - (std::chrono::steady_clock::now() - start);
          const auto slice_seconds =
              remaining.count() < 0.05 ? remaining.count() : 0.05;
          if (slice_seconds > 0.0) {
            const auto slice = std::chrono::duration<double>(slice_seconds);
            std::this_thread::sleep_for(slice);
          }
        }
      }

      current_system = read_system_snapshot();
      current_processes = enumerate_processes();
    }

    auto rows = build_rows(previous_processes, current_processes,
                           previous_system, current_system, effective);
    sort_rows(rows, effective.sort_by);

    int max_rows = -1;
    if (interactive) {
      const ConsoleView view = console_view(out);
      clear_interactive_screen(out, view);

      const int summary_lines = effective.no_headers ? 0 : 6;
      const int table_header_lines = effective.no_headers ? 0 : 1;
      max_rows = std::max(
          0, static_cast<int>(view.height) - summary_lines - table_header_lines -
                 1);
    }

    const double cpu_usage = calculate_system_cpu(previous_system, current_system);
    if (!effective.no_headers) {
      print_summary(current_system, current_processes.size(), cpu_usage, out,
                    interactive);
    }
    print_table(rows, effective, out, interactive, max_rows);
    std::cout.flush();

    previous_system = current_system;
    previous_processes = current_processes;
    ++iteration;

    if (!effective.batch_mode && !interactive) {
      if (_kbhit()) {
        const int ch = _getch();
        bool quit = false;
        handle_interactive_key(ch, effective, quit);
        if (quit) {
          break;
        }
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
  out.pids = parse_pid_list(options->pids);
  out.sort_by = options->sort_by ? upper(options->sort_by) : L"CPU";
  out.user_filter = options->user ? upper(options->user) : L"";
  out.no_headers = options->no_headers != 0;
  out.show_command = options->show_command != 0;
  out.show_threads = options->show_threads != 0;
  out.ignore_idle = options->ignore_idle != 0;
  out.width = options->width > 0 ? options->width : 120;
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
