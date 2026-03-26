#ifndef CONSOLE_H
#define CONSOLE_H

#include <stddef.h>  // size_t

size_t console_getLogs(char* buf, size_t bufSize);

void console_init();
void console_patch();

#endif // CONSOLE_H
