#ifndef DEMO_H
#define DEMO_H

#include <cstddef>

void demo_scheduleCloseAfterUpload();
bool demo_getDemoForUpload(char* demoFileNameBuffer = nullptr, size_t bufferSize = 0, char* demoMarkerFileBuffer = nullptr, size_t demoMarkerFileBufferSize = 0);

void demo_drawing();
void demo_unload();
void demo_frame();
void demo_init();
void demo_patch();

#endif
