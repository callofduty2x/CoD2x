#ifndef DEBUG_PIPE_H
#define DEBUG_PIPE_H

// Debug-only named-pipe control channel (\\.\pipe\cod2x-<pid>) for automated tests, see debug_pipe.cpp.
// In release builds the functions are empty and no pipe code is compiled in.

// Debug builds only - call them inside #if DEBUG
void debug_pipe_commandError(const char* msg);
void debug_pipe_crash(const char* text);

// Empty in release builds
void debug_pipe_frame();
void debug_pipe_unload();
void debug_pipe_init();
void debug_pipe_patch();

#endif
