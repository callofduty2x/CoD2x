#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>  // size_t

// Capacity of the crash-safe console mirror buffer. Callers that want to capture the
// whole console history (e.g. the crash handler) should size their buffer at least this big.
#define CONSOLE_MIRROR_CAPACITY 65536

size_t console_getLogs(char* buf, size_t bufSize);

void console_init();
void console_patch();

#endif // CONSOLE_H
