#ifndef REGISTRY_H
#define REGISTRY_H

#include <windows.h>

typedef bool (*registry_gamma_enum_callback_t)(const char* deviceName, const WORD ramp[3][256], void* context);

bool registry_gamma_getSaved(const char* deviceName, WORD ramp[3][256]);
bool registry_gamma_save(const char* deviceName, const WORD ramp[3][256]);
void registry_gamma_clear(const char* deviceName);
void registry_gamma_forEachSaved(registry_gamma_enum_callback_t callback, void* context);

void registry_frame();
void registry_init();

#endif // REGISTRY_H