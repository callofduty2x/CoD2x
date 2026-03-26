#ifndef HOOK_H
#define HOOK_H

extern unsigned int gfx_module_addr;

int hook_gfxDll();
void hook_CL_Init();
void hook_SV_Init();
void hook_Com_Init(const char* cmdline);
bool hook_patch();

#endif