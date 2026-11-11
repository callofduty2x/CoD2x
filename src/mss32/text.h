#ifndef TEXT_H
#define TEXT_H

#include <stdint.h>

// Localizes the text (a localization reference like "WEAPON_KAR98K", or a text with references), the original hooked in CG_ServerCommand
char* SEH_LocalizeTextMessage(const char* pszInputBuffer, const char* pszMessageType, int32_t errType);

void text_frame();
void text_unload();
void text_init();
void text_patch();

#endif
