#include "downloading.h"

#include "shared.h"


int EventListTimerHandler (void * timer, void * param) {

    // CoD2x: Fix crash at 0x0054e658 when trying to access param->timeouts
    if (param == NULL || timer == NULL || IsBadReadPtr(param, 0x20)) {
        Com_Printf("EventListTimerHandler: preventing crash\n");
        return -1; // HT_ERROR
    }

    int ret;
    ASM_CALL(RETURN(ret), 0x0054e650, 2, PUSH(timer), PUSH(param));

    return ret;
}


/** Called before the entry point to patch memory for downloading features. */
void downloading_patch() {
    patch_int32(0x0054e90c + 1, (unsigned int)EventListTimerHandler); // push EventListTimerHandler

    // Downloading screen
    patch_float(0x00539351 + 1, 0.35f); // font size (0.5f original)
    patch_float(0x005c4390, 210.0f);    // Y1 (210.0f original)
    patch_float(0x005c438c, 230.0f);    // Y2 (235.0f original)
    patch_float(0x005c4388, 250.0f);    // Y3 (260.0f original)
}