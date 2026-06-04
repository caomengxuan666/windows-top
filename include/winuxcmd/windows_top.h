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

#ifndef WINUXCMD_WINDOWS_TOP_H
#define WINUXCMD_WINDOWS_TOP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct windows_top_options {
  int batch_mode;
  int iterations;
  double delay_seconds;
  const wchar_t* pids;
  const wchar_t* sort_by;
  const wchar_t* user;
  int no_headers;
  int show_command;
  int show_threads;
  int ignore_idle;
  int width;
} windows_top_options;

const wchar_t* windows_top_version(void);
int windows_top_run(int argc, const wchar_t* const* argv);
int windows_top_run_with_options(const windows_top_options* options);

#ifdef __cplusplus
}
#endif

#endif
