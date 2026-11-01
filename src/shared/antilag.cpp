#include "shared.h"
#include "antilag.h"
#include "cod2_common.h"
#include "cod2_dvars.h"

namespace {

// CoD2 1.3: Bullet_Fire's sole call to the original rewind preparation routine.
// Both platforms pass (gameTime, savedPositions) on the stack, caller cleanup.
constexpr unsigned int rewindCall = ADDR(0x00527b88, 0x0812049d);
constexpr unsigned int rewindOriginal = ADDR(0x00527520, 0x0811fc76);
constexpr unsigned int gameTimeAddress = ADDR(0x0193a96c, 0x0864c56c);
constexpr unsigned int nativeAntilagAddress = ADDR(0x017d03c0, 0x087a24cc);
const unsigned char expectedCall[5] = {
    0xe8, WL(0x93, 0xd4), WL(0xf9, 0xf7), 0xff, 0xff
};

dvar_t* maxRewind = nullptr;
bool installed = false;

void __cdecl rewindCapped(int gameTime, void* savedPositions)
{
    const auto nativeAntilag = *reinterpret_cast<dvar_t**>(nativeAntilagAddress);
    if (maxRewind && nativeAntilag && nativeAntilag->value.boolean) {
        const auto now = *reinterpret_cast<uint32_t*>(gameTimeAddress);
        const auto shot = static_cast<uint32_t>(gameTime);
        const auto age = static_cast<int32_t>(now - shot);
        // Unsigned subtraction preserves the engine's millisecond clock at wrap.
        // Recent/future commands keep their original timestamp. Only the rewind
        // request changes; movement timing, bullet tracing and restore stay stock.
        if (age > maxRewind->value.integer)
            gameTime = static_cast<int32_t>(now - maxRewind->value.integer);
    }

    reinterpret_cast<void (__cdecl*)(int, void*)>(rewindOriginal)(gameTime, savedPositions);
}

} // namespace

void antilag_init()
{
    if (!installed) {
        Com_Printf("CoD2x: anti-lag cap unavailable (rewind call signature mismatch).\n");
        return;
    }
    // Server-owned, visible in server info, changeable live without cheats/restart.
    maxRewind = Dvar_RegisterInt("g_antilagMaxMs", 200, 120, 800, DVAR_SERVERINFO);
}

void antilag_patch()
{
    if (installed)
        return;
    if (memcmp(reinterpret_cast<const void*>(rewindCall), expectedCall, sizeof(expectedCall)) != 0) {
        logger_add("Anti-lag cap: unexpected rewind call at 0x%08x; leaving it unchanged.", rewindCall);
        return;
    }
    patch_call(rewindCall, reinterpret_cast<unsigned int>(rewindCapped));
    installed = true;
}

void antilag_unload()
{
    if (installed) {
        patch_call(rewindCall, rewindOriginal);
        installed = false;
    }
    maxRewind = nullptr;
}
