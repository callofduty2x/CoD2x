#include "playerinfo.h"

#include "shared.h"
#include "cod2_entity.h"
#include "cod2_player.h"

#define bg_weaponDefs           (*((void* (*)[0x80])ADDR(0x01728200, 0x086270a0)))  // BG_GetWeaponDef is inlined in the Windows build
#define bg_iNumWeapons          (*((int*)ADDR(0x019a1c74, 0x08627080)))              // weapons 1 .. bg_iNumWeapons (BG_GetNumWeapons)

// WeaponDef fields (see BG_GetFirstAvailableOffhand, Win 0x004f1da0, Linux 0x080ebd6e)
#define WeaponDef_offhandClass(def) (*(int*)((char*)(def) + 0x084))  // 0 = not an offhand weapon, 1 = frag, 2 = smoke
#define WeaponDef_ammoIndex(def)    (*(int*)((char*)(def) + 0x1c8))  // index to ps.ammo (reserve)
#define WeaponDef_clipIndex(def)    (*(int*)((char*)(def) + 0x1d0))  // index to ps.ammoclip

// gclient_t starts with the playerState, the session follows it (ClientEndFrame)
#define gclient_sessionState(client)    (*(int*)((char*)(client) + 0x26a8))   // 0 = playing, 1 = dead, 2 = spectator, 3 = intermission
#define SESS_STATE_PLAYING              0

static_assert(offsetof(gentity_t, health) == 0x194, "gentity_t::health");
static_assert(offsetof(playerState_t, weaponslots) == 0x554, "playerState_t::weaponslots");
static_assert(offsetof(entityState_t, playerHealth) == 0x88, "entityState_t::playerHealth");
static_assert(offsetof(entityState_t, playerInfo) == 0xdc, "entityState_t::playerInfo");



/** Number of grenades of the offhand class the player has, the same rule as BG_GetFirstAvailableOffhand (owned weapon, clip + reserve). */
static int PlayerInfo_CountOffhand(playerState_t* ps, int offhandClass) {
    int count = 0;
    for (int weapon = 1; weapon <= bg_iNumWeapons && weapon < (int)sizeof(ps->weapons) * 8; weapon++) {
        void* def = bg_weaponDefs[weapon];
        if (!def || WeaponDef_offhandClass(def) != offhandClass)
            continue;
        if (!((unsigned int)ps->weapons[weapon >> 5] & (1u << (weapon & 0x1f))))
            continue;
        count += ps->ammoclip[WeaponDef_clipIndex(def)] + ps->ammo[WeaponDef_ammoIndex(def)];
    }
    return count;
}

/**
 * Writes the player info (health, weapons, grenades) into the entity state of every player, once per server frame after G_RunFrame (that already
 * copied the playerState into the entity state in ClientEndFrame). Always sent; players that are not playing get no info.
 * Introduced in CoD2x protocol 7.
 */
void PlayerInfo_ServerFrame() {
    for (int i = 0; i < 64; i++) {
        gentity_t* ent = &g_entities[i];
        if (!ent->client || !ent->r.inuse)
            continue;

        playerState_t* ps = (playerState_t*)ent->client;
        if (ent->s.eType != ET_PLAYER || gclient_sessionState(ent->client) != SESS_STATE_PLAYING || ent->health <= 0) {
            PlayerInfo_Clear(&ent->s);
            continue;
        }

        playerInfo_t info;
        info.health = ent->health;
        info.primaryWeapon = (unsigned char)ps->weaponslots[1];
        info.secondaryWeapon = (unsigned char)ps->weaponslots[2];
        info.fragGrenades = PlayerInfo_CountOffhand(ps, PLAYERINFO_OFFHAND_FRAG);
        info.smokeGrenades = PlayerInfo_CountOffhand(ps, PLAYERINFO_OFFHAND_SMOKE);
        PlayerInfo_Pack(&ent->s, &info);
    }
}
