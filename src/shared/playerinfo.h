#ifndef PLAYERINFO_H
#define PLAYERINFO_H

#include "cod2_entity.h"

// Player info - health, weapons and grenades of every player, sent by the server to all clients in unused entityState fields of the player
// entities (ET_PLAYER), so it's in every snapshot, in the killcam archive (both fields are in archivedEntityFields) and in demos.
// Stock clients ignore these fields for players.
// See docs/network-protocol.md section 7.4.
//
//   es.playerHealth (surfType, 8 bits, netfield 15)  = health, clamped to 0..255
//   es.playerInfo   (dmgFlags, 32 bits, netfield 58) =
//       bits  0- 6  primary weapon   (ps.weaponslots[1])
//       bits  7-13  secondary weapon (ps.weaponslots[2])
//       bits 14-17  frag grenades    (offhand class 1, clip + reserve, clamped 0..15)
//       bits 18-21  smoke grenades   (offhand class 2, clip + reserve, clamped 0..15)
//       bits 22-23  reserved (0)
//       bits 24-27  format version   (1)
//       bits 28-31  signature        (0xC)
//
// A valid word is always 0xC1xxxxxx. Stock servers (and older CoD2x servers) leave both fields 0 for players, so the data is used only with
// the signature - not by g_cod2x, which is 0 in demo playback.
//
// Introduced in CoD2x protocol 7 (1.4.7.x): the server sends it to every client, and protocol 7 servers accept only protocol >= 7 clients.

#define PLAYERINFO_PROTOCOL         7                   // CoD2x protocol (g_cod2x) of the servers that send the player info
#define PLAYERINFO_SIGNATURE        0xC1                // bits 24-31 of es.playerInfo: signature 0xC + version 1
#define PLAYERINFO_MAX_HEALTH       100                 // max health is not sent, the stock value

#define PLAYERINFO_OFFHAND_FRAG     1                   // weaponDef offhandClass (BUTTON_FRAG in PM_Weapon_CheckForOffHand)
#define PLAYERINFO_OFFHAND_SMOKE    2                   // weaponDef offhandClass (BUTTON_SMOKE)

struct playerInfo_t {
    int health;             // 0 .. 255
    int primaryWeapon;      // weapon index, 0 = none
    int secondaryWeapon;    // weapon index, 0 = none
    int fragGrenades;       // 0 .. 15
    int smokeGrenades;      // 0 .. 15
};

/** Packs the player info into the entity state of the player. */
inline void PlayerInfo_Pack(entityState_t* es, const playerInfo_t* info) {
    int health = info->health < 0 ? 0 : (info->health > 255 ? 255 : info->health);
    int frags = info->fragGrenades < 0 ? 0 : (info->fragGrenades > 15 ? 15 : info->fragGrenades);
    int smokes = info->smokeGrenades < 0 ? 0 : (info->smokeGrenades > 15 ? 15 : info->smokeGrenades);

    es->playerHealth = health;
    es->playerInfo = (int)(
        ((unsigned int)(info->primaryWeapon & 0x7f)) |
        ((unsigned int)(info->secondaryWeapon & 0x7f) << 7) |
        ((unsigned int)frags << 14) |
        ((unsigned int)smokes << 18) |
        ((unsigned int)PLAYERINFO_SIGNATURE << 24));
}

/** Clears the player info in the entity state (no signature, the client shows nothing). */
inline void PlayerInfo_Clear(entityState_t* es) {
    es->playerHealth = 0;
    es->playerInfo = 0;
}

/** Reads the player info from the entity state of the player. False if the state has no valid player info (stock server, old demo). */
inline bool PlayerInfo_Unpack(const entityState_t* es, playerInfo_t* info) {
    unsigned int bits = (unsigned int)es->playerInfo;
    if ((bits >> 24) != PLAYERINFO_SIGNATURE)
        return false;
    info->health = es->playerHealth & 0xff;
    info->primaryWeapon = bits & 0x7f;
    info->secondaryWeapon = (bits >> 7) & 0x7f;
    info->fragGrenades = (bits >> 14) & 0xf;
    info->smokeGrenades = (bits >> 18) & 0xf;
    return true;
}

void PlayerInfo_ServerFrame();

#endif
