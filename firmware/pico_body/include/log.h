// log.h — formatted debug output.
// ---------------------------------------------------------------------------
// Under `framework = picosdk` the platform's builder defaults stdio to USB, so
// printf() goes out of the USB port and uses no GPIO at all. That is what keeps
// UART0 (GP16/GP17) free for the link, and keeps GP0/GP1 -- which are DRV1_PWMA
// and DRV1_AIN1 on this board -- away from any console.
//
// Call stdio_init_all() once in main() before the first logf().
// ---------------------------------------------------------------------------
#pragma once

#include <stdarg.h>
#include <stdio.h>

// Print a formatted line. The fflush matters: USB CDC output is buffered, and
// without it a crash between two lines loses the one that explains why.
inline void logf(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  fflush(stdout);
}
