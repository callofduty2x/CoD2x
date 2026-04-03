#include "discord.h"

#include "../shared/cod2_common.h"
#include "../shared/cod2_client.h"
#include "../shared/cod2_dvars.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>

#ifdef COD2X_WITH_DISCORD
#include "../shared/version.h"
#include <discord-rpc.hpp>
#include <string>
#ifndef COD2X_DISCORD_CLIENT_ID
#define COD2X_DISCORD_CLIENT_ID ""
#endif
static bool g_discord_rpc_active;

static void discord_rpc_init(void) {
	if (g_discord_rpc_active)
		return;
	const char *cid = COD2X_DISCORD_CLIENT_ID;
	if (cid == nullptr || std::strlen(cid) == 0)
		return;

	discord::RPCManager &rpc = discord::RPCManager::get();
	rpc.setClientID(std::string(cid));
	rpc.initialize();

	discord::Presence &pr = rpc.getPresence();
	pr.setActivityType(discord::ActivityType::Game);
	pr.setDetails("Call of Duty 2 X");
	pr.setState("");
	pr.refresh();

	g_discord_rpc_active = true;
}

static void discord_rpc_set_game(const char *details, const char *state, const char *large_image_key,
				 const char *large_image_text, int64_t start_timestamp_unix) {
	if (!g_discord_rpc_active)
		return;
	discord::Presence &pr = discord::RPCManager::get().getPresence();
	if (details)
		pr.setDetails(details);
	if (state)
		pr.setState(state);
	if (large_image_key && large_image_key[0])
		pr.setLargeImageKey(large_image_key);
	if (large_image_text && large_image_text[0])
		pr.setLargeImageText(large_image_text);
	if (start_timestamp_unix > 0)
		pr.setStartTimestamp(start_timestamp_unix);
	else
		pr.setStartTimestamp(0);
	pr.refresh();
}

static void discord_rpc_shutdown(void) {
	if (!g_discord_rpc_active)
		return;
	discord::RPCManager &rpc = discord::RPCManager::get();
	/* With DISCORD_DISABLE_IO_THREAD, update() drains the queue on the calling thread; do not
	   sleep here (DllMain / process shutdown). */
	rpc.clearPresence();
	rpc.update();
	rpc.shutdown();
	g_discord_rpc_active = false;
}

static void discord_rpc_frame(void) {
	if (!g_discord_rpc_active)
		return;
	discord::RPCManager::get().update();
}

#if defined(__linux__) && !defined(_WIN32)
namespace {
struct DiscordShutdownAtExit {
	~DiscordShutdownAtExit() { discord_rpc_shutdown(); }
};
static DiscordShutdownAtExit discord_at_exit;
}
#endif

#else
static void discord_rpc_init(void) {}
static void discord_rpc_set_game(const char *, const char *, const char *, const char *, int64_t) {}
static void discord_rpc_shutdown(void) {}
static void discord_rpc_frame(void) {}
#endif

#ifndef DISCORD_LARGE_IMAGE_KEY
#define DISCORD_LARGE_IMAGE_KEY "cod2_fw"
#endif
#ifndef DISCORD_LARGE_IMAGE_TEXT
#define DISCORD_LARGE_IMAGE_TEXT "Call of Duty 2 X"
#endif

enum { DISCORD_LINE = 128 };

static void discord_strip_q3_colors(char *out, const char *in, size_t outsz) {
	if (!out || outsz == 0)
		return;
	if (!in) {
		out[0] = 0;
		return;
	}
	size_t j = 0;
	for (; *in && j + 1 < outsz; ++in) {
		if (*in == Q_COLOR_ESCAPE && in[1])
			++in;
		else
			out[j++] = *in;
	}
	out[j] = 0;
}

static void discord_map_basename(char *out, const char *in, size_t outsz) {
	if (!out || outsz == 0)
		return;
	if (!in || !*in) {
		out[0] = 0;
		return;
	}
	const char *last = in;
	for (const char *p = in; *p; ++p) {
		if (*p == '/' || *p == '\\')
			last = p + 1;
	}
	Q_strncpyz(out, last, (int)outsz);
}

/** CoD4-style line, e.g. "SD - crossfire" (gametype in uppercase). */
static void discord_fmt_details_gametype_map(char *out, size_t outsz, const char *gametype,
					     const char *map) {
	char g[64];
	Q_strncpyz(g, gametype && gametype[0] ? gametype : "?", sizeof g);
	for (char *p = g; *p; ++p) {
		if (*p >= 'a' && *p <= 'z')
			*p = (char)(*p - 'a' + 'A');
	}
	snprintf(out, outsz, "%s - %s", g, map && map[0] ? map : "?");
}

#if COD2X_WIN32
/** Player count from valid client slots (CS_SERVERINFO often omits "clients"). */
static int discord_client_player_count(void) {
	int n = 0;
	for (int i = 0; i < 64; i++) {
		if (clientInfo[i].infoValid)
			n++;
	}
	return n;
}

static void discord_refresh_presence(void) {
	char details[DISCORD_LINE] = {0};
	char state[DISCORD_LINE] = {0};
	bool push = false;
	int64_t start_ts = 0;

	static int64_t discord_activity_start = 0;

	const char *cs = CL_GetConfigString(CS_SERVERINFO);
	if (!cs)
		cs = "";

	char mapraw[128];
	char map[128];
	char gametype[64];

	const char *mr = Info_ValueForKey(cs, "mapname");
	Q_strncpyz(mapraw, mr ? mr : "", sizeof mapraw);
	discord_map_basename(map, mapraw, sizeof map);
	discord_strip_q3_colors(map, map, sizeof map);

	const char *gt = Info_ValueForKey(cs, "g_gametype");
	Q_strncpyz(gametype, gt ? gt : "", sizeof gametype);

	const char *cl_inf = Info_ValueForKey(cs, "clients");
	const char *mx = Info_ValueForKey(cs, "sv_maxclients");
	char mx_fallback[16];
	if ((!mx || !mx[0]) && sv_maxclients) {
		snprintf(mx_fallback, sizeof mx_fallback, "%d", sv_maxclients->value.integer);
		mx = mx_fallback;
	}

	const bool have_map_meta = (map[0] != 0 && gametype[0] != 0);

	if (clientState == CLIENT_STATE_DISCONNECTED) {
		discord_activity_start = 0;
		Q_strncpyz(details, "Menu", sizeof details);
		Q_strncpyz(state, DISCORD_LARGE_IMAGE_TEXT, sizeof state);
		push = true;
		start_ts = 0;
	} else {
		if (have_map_meta && discord_activity_start == 0)
			discord_activity_start = (int64_t)time(nullptr);
		start_ts = discord_activity_start;
	}

	if (clientState != CLIENT_STATE_DISCONNECTED) {
		switch (clientState) {
		case CLIENT_STATE_CINEMATIC:
			/* Empty strings do not reliably update Discord; previous state may linger. */
			Q_strncpyz(details, "Menu", sizeof details);
			Q_strncpyz(state, "Intro / main menu", sizeof state);
			push = true;
			start_ts = 0;
			break;
		case CLIENT_STATE_AUTHORIZING:
			if (have_map_meta)
				discord_fmt_details_gametype_map(details, sizeof details, gametype, map);
			else
				Q_strncpyz(details, "Call of Duty 2 X", sizeof details);
			Q_strncpyz(state, "Authorizing server", sizeof state);
			push = true;
			break;
		case CLIENT_STATE_CONNECTING:
		case CLIENT_STATE_CHALLENGING:
		case CLIENT_STATE_CONNECTED:
		case CLIENT_STATE_LOADING:
		case CLIENT_STATE_PRIMED:
			if (have_map_meta) {
				discord_fmt_details_gametype_map(details, sizeof details, gametype, map);
				const char *phase = "Joining server";
				if (clientState == CLIENT_STATE_CONNECTING)
					phase = "Connecting to a server";
				else if (clientState == CLIENT_STATE_LOADING)
					phase = "Loading level";
				Q_strncpyz(state, phase, sizeof state);
			} else {
				Q_strncpyz(details, "Call of Duty 2 X", sizeof details);
				Q_strncpyz(state, "Connecting to a server", sizeof state);
			}
			push = true;
			break;
		case CLIENT_STATE_ACTIVE: {
			discord_fmt_details_gametype_map(details, sizeof details, gametype, map);
			char cl_buf[16];
			const char *cl_show = (cl_inf && cl_inf[0]) ? cl_inf : nullptr;
			if (!cl_show) {
				snprintf(cl_buf, sizeof cl_buf, "%d", discord_client_player_count());
				cl_show = cl_buf;
			}
			snprintf(state, sizeof state, "Playing on a Server (%s of %s)", cl_show,
				 (mx && mx[0]) ? mx : "?");
			push = true;
			break;
		}
		default:
			if (have_map_meta) {
				discord_fmt_details_gametype_map(details, sizeof details, gametype, map);
				Q_strncpyz(state, "In game", sizeof state);
				push = true;
			}
			break;
		}
	}

	if (push)
		discord_rpc_set_game(details, state, DISCORD_LARGE_IMAGE_KEY, DISCORD_LARGE_IMAGE_TEXT,
				     start_ts);
}
#else
static void discord_refresh_presence(void) {}
#endif

void discord_patch(void) {
}

void discord_init(void) {
	if (dedicated->value.integer != 0)
		return;
	discord_rpc_init();
}

void discord_frame(void) {
	if (dedicated->value.integer != 0)
		return;
	discord_refresh_presence();
	discord_rpc_frame();
}

void discord_unload(void) {
	discord_rpc_shutdown();
}
