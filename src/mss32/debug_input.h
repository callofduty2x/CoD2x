#ifndef DEBUG_INPUT_H
#define DEBUG_INPUT_H

// Debug-only console commands that play the client for automated tests: aim at players, look, move, fire, press keys,
// type text and click menu items - see debug_input.cpp. In release builds the functions are empty.

void debug_input_frame();
void debug_input_unload();
void debug_input_init();

#endif
