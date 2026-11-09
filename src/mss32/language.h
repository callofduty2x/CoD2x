#ifndef LANGUAGE_H
#define LANGUAGE_H

// Name of the game language ("english", "czech", ...)
const char* language_getName(int language);

void language_frame();
void language_init();
void language_patch();

#endif
