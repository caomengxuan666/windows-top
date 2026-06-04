#ifndef WINDOWS_TOP_WINDOWS_TOP_H
#define WINDOWS_TOP_WINDOWS_TOP_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct windows_top_options {
  int batch_mode;
  int iterations;
  double delay_seconds;
  unsigned long pid;
  const wchar_t* sort_by;
  int no_headers;
} windows_top_options;

const wchar_t* windows_top_version(void);
int windows_top_run(int argc, const wchar_t* const* argv);
int windows_top_run_with_options(const windows_top_options* options);

#ifdef __cplusplus
}
#endif

#endif

