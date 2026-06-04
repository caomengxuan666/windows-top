#include "windows_top/windows_top.h"

int wmain(int argc, wchar_t** argv) {
  return windows_top_run(argc, const_cast<const wchar_t* const*>(argv));
}

