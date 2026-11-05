#include "gsc_player.h"

#include "shared.h"
#include "cod2_common.h"
#include "cod2_script.h"
#include "cod2_server.h"
#include "cod2_player.h"


/* Get the IP address of a player */
void gsc_player_getip(scr_entref_t id)
{
	if (id.entnum >= MAX_CLIENTS)
	{
		Scr_Error(va("entity %i is not a player", id.entnum));
		Scr_AddUndefined();
		return;
	}

	client_t *client = &svs_clients[id.entnum];

	char tmp[16];
	snprintf(tmp, sizeof(tmp), "%d.%d.%d.%d", client->netchan.remoteAddress.ip[0], client->netchan.remoteAddress.ip[1], client->netchan.remoteAddress.ip[2], client->netchan.remoteAddress.ip[3]);

	Scr_AddString(tmp);
}


/** Get the 32 character long HWID2 of a player */
void gsc_player_playerGetHWID(scr_entref_t ref) {
	int id = ref.entnum;

	if ( id >= MAX_CLIENTS )
	{
		Scr_Error(va("entity %d is not a player", id));
		Scr_AddUndefined();
		return;
	}

	client_t *client = &svs_clients[id];

	const char* HWID2 = Info_ValueForKey(client->userinfo, "cl_hwid2");

	// Dont use this check, as its empty for bots and for clients it should not happen as its validated on connect
	/*if (HWID2 == NULL || strlen(HWID2) != 32)
	{
		Scr_Error(va("client %d has no HWID2", id)); // should never happen, as its validated on client connect
		Scr_AddUndefined();
		return;
	}*/

	Scr_AddString(HWID2);
}

/**
 * Get the CDKey hash of a player.
 * The CDKey hash is a 32 character long string sent by the client during connection.
 * It is the MD5 hash of the CDKey.
 * If the server is not cracked, this hash sent to the authorization server to validate the CDKey.
 * This hash can not be trusted as the client can send any value.
 * User can change the hash by setting different CDKey in the game options.
 * Call getAuthorizationStatus() to check if the CDKey is valid.
 */ 
void gsc_player_playerGetCDKeyHash(scr_entref_t ref) {
	int id = ref.entnum;

	if ( id >= MAX_CLIENTS )
	{
		Scr_Error(va("entity %d is not a player", id));
		Scr_AddUndefined();
		return;
	}

	client_t *client = &svs_clients[id];

	Scr_AddString(client->clientPBguid);
}


/**
 * Get the authorization status of a player's CDKey.
 * The status is a string sent by the authorization server during client connection.
 * Is "KEY_IS_GOOD" if the CDKey is valid.
 * Is "INVALID_CDKEY" if the player is using invalid CDKey, or key is in use by another player.
 * Is "CLIENT_UNKNOWN_TO_AUTH" if the authorization server does not know the client (the client can not reach the auth server).
 * Is "BANNED_CDKEY" if the CDKey is banned.
 * If the server is cracked (sv_cracked 1), the status can be any string, as the server will accept any CDKey.
 * Is empty if the player is connected via LAN network.
 * Is empty if the game is running locally from listen server.
 */
void gsc_player_playerGetAuthorizationStatus(scr_entref_t ref) {
	int id = ref.entnum;

	if ( id >= MAX_CLIENTS )
	{
		Scr_Error(va("entity %d is not a player", id));
		Scr_AddUndefined();
		return;
	}

	client_t *client = &svs_clients[id];

	// In CoD2x, we store the authorization status in PBguid field of the challenge structure
	Scr_AddString(client->PBguid);
}


/* Get the player's view origin */
void gsc_player_getViewOrigin(scr_entref_t ref) {
	int id = ref.entnum;

	if ( id >= MAX_CLIENTS )
	{
		Scr_Error(va("entity %d is not a player", id));
		Scr_AddUndefined();
		return;
	}

	gentity_t* ent = &g_entities[id];

	// Get the player's view origin
	vec3_t viewOrigin;
	G_GetPlayerViewOrigin(ent, viewOrigin);

	Scr_AddVector(viewOrigin);
}


/** Get the player's stance */
void gsc_player_getStance(scr_entref_t ref) {
	int id = ref.entnum;

	if ( id >= MAX_CLIENTS )
	{
		Scr_Error(va("entity %d is not a player", id));
		Scr_AddUndefined();
		return;
	}
	
	playerState_t* ps = SV_GetPlayerStateByNum(id);

	if (ps->pm_flags & PMF_CROUCH)
		Scr_AddString("crouch");
	else if (ps->pm_flags & PMF_PRONE)
		Scr_AddString("prone");
	else
		Scr_AddString("stand");
}


/** Get the player's stance */
void gsc_player_isUsingTurret(scr_entref_t ref) {
	int id = ref.entnum;

	if ( id >= MAX_CLIENTS )
	{
		Scr_Error(va("entity %d is not a player", id));
		Scr_AddUndefined();
		return;
	}

	gentity_t* ent = &g_entities[id];

	if (ent->s.eFlags & ENTITY_FLAG_MG)
		Scr_AddBool(true);
	else
		Scr_AddBool(false);
}


// Original kick() function, called when no reason is passed
static xfunction_t gsc_player_kick_original = NULL;

// Players scheduled to be kicked with reason in next frame
static bool gsc_player_kickPending[MAX_CLIENTS] = {};
static char gsc_player_kickReasons[MAX_CLIENTS][256];

/**
 * Override of original kick(<entity number>) function with new optional reason parameter.
 * Example: kick(self getEntityNumber(), "was kicked for being AFK");
 *  - Without reason (or with empty or non-string reason) the original function is called (player is temporary banned for sv_kickBanTime).
 *  - With reason, the reason is shown in the kicked player's disconnect dialog and printed to all players as "<name> <reason>".
 *    No temporary ban is applied, the player can reconnect right away.
 *    Like the original function, the player is dropped at the start of the next server frame (not inside the script call).
 * Servers without this CoD2x version ignore the reason parameter, so scripts can use it without version check.
 */
void gsc_player_kick() {
	// No reason, reason is not a string or is empty - original behavior
	if (Scr_GetNumParam() < 2 || strcmp(Scr_GetTypeName(1), "string") != 0 || Scr_GetString(1)[0] == '\0')
	{
		if (gsc_player_kick_original)
			gsc_player_kick_original();
		return;
	}

	int id = Scr_GetInt(0);

	if (id < 0 || id >= sv_maxclients->value.integer)
	{
		Scr_Error(va("kick: invalid client number %i", id));
		return;
	}

	client_t *client = &svs_clients[id];

	// The host of a listen server can not be kicked (same as original function)
	if (client->state < CS_CONNECTED || client->netchan.remoteAddress.type == NA_LOOPBACK)
		return;

	// Copy the reason, double quotes would break the quoted server command
	char* reason = gsc_player_kickReasons[id];
	snprintf(reason, sizeof(gsc_player_kickReasons[id]), "%s", Scr_GetString(1));
	for (char* c = reason; *c; c++) {
		if (*c == '"') *c = '\'';
	}
	gsc_player_kickPending[id] = true;
}

/** Called when kick() function is being resolved while scripts are compiled. Returns the override. */
xfunction_t gsc_player_kick_override(xfunction_t original) {
	gsc_player_kick_original = original;
	return gsc_player_kick;
}


/** Called every frame on frame start. Drops players kicked with reason outside of script execution. */
void gsc_player_frame() {
	for (int i = 0; i < MAX_CLIENTS; i++) {
		if (!gsc_player_kickPending[i])
			continue;
		gsc_player_kickPending[i] = false;

		client_t *client = &svs_clients[i];
		if (client->state >= CS_CONNECTED)
			SV_DropClient(client, gsc_player_kickReasons[i]);
	}
}
