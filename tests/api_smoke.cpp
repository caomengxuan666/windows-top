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

#include "winuxcmd/windows_top.h"

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
  options.show_threads = 1;
  options.width = 80;

  return windows_top_run_with_options(&options);
}
