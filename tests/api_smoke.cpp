#include "windows_top/windows_top.h"

#include <cwchar>

int main() {
  if (std::wcscmp(windows_top_version(), L"0.1.0") != 0) {
    return 1;
  }

  windows_top_options options{};
  options.batch_mode = 1;
  options.iterations = 1;
  options.delay_seconds = 0.05;
  options.sort_by = L"PID";

  return windows_top_run_with_options(&options);
}

