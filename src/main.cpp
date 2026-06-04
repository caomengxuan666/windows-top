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

int wmain(int argc, wchar_t** argv) {
  return windows_top_run(argc, const_cast<const wchar_t* const*>(argv));
}
