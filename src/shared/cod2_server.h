#ifndef COD2_SERVER_H
#define COD2_SERVER_H

#include "shared.h"
#include "assembly.h"
#include "cod2_shared.h"
#include "cod2_dvars.h"
#include "cod2_player.h"
#include "cod2_net.h"

#define MAX_CHALLENGES 1024
#define MAX_CLIENTS 64

#define svs_authorizeAddress 					(*((netaddr_s*)(ADDR(0x00d52770, 0x084400f0))))
#define svs_challenges 							(*((challenge_t (*)[MAX_CHALLENGES])(ADDR(0x00d3575c, 0x084230dc))))
#define svs_time 								(*((int*)(ADDR(0x00d35704, 0x08423084))))
#define svs_nextHeartbeatTime 					(*((int*)(ADDR(0x00d35754, 0x084230d4))))
#define svs_nextStatusResponseTime 				(*((int*)(ADDR(0x00d35758, 0x084230d8))))
#define svs_clients                             (*(client_t (*)[MAX_CLIENTS])(*(void**)(ADDR(0x00d3570c, 0x0842308c))))



typedef struct
{
	netaddr_s adr;
	int challenge;
	int time;
	int pingTime;
	int firstTime;
	int firstPing;
	int connected;
	int guid;
	char PBguid[33];
	char clientPBguid[33];
} challenge_t;
static_assert(sizeof(challenge_t) == 0x74, "sizeof(challenge_t)");



typedef struct
{
	playerState_t ps;
	int	num_entities;
	int	num_clients;
	int	first_entity;
	int	first_client;
	int messageSent;
	int messageAcked;
	int	messageSize;
} clientSnapshot_t;

typedef enum
{
	CS_FREE,
	CS_ZOMBIE,
	CS_CONNECTED, // SV_DirectConnect successfully called
	CS_PRIMED,
	CS_ACTIVE
} clientState_t;


typedef struct client_s
{
    clientState_t state;
    int sendAsActive;
    char const* dropReason;
    char userinfo[0x400];
    byte reliableCommandInfo[0x408][0x80];
    int reliableSequence;
    int reliableAcknowledge;
    int reliableSent;
    int messageAcknowledge;
    int gamestateMessageNum;
    int challenge;
    usercmd_t lastUsercmd;
    int lastClientCommand;
    char lastClientCommandString[0x400];
    gentity_t* gentity;
    char name[0x20];
    char downloadName[0x40];
    int download;
    int downloadSize;
    int downloadCount;
    int downloadClientBlock;
    int downloadCurrentBlock;
    int downloadXmitBlock;
    uint8_t* downloadBlocks[0x8];
    int downloadBlockSize[0x8];
    int downloadEOF;
    int downloadSendTime;
    char downloadURL[0x100];
    int wwwOk;
    int downloadingWWW;
    int clientDownloadingWWW;
    int wwwFallback;
    int deltaMessage;
    int nextReliableTime;
    int lastPacketTime;
    int lastConnectTime;
    int nextSnapshotTime;
    int rateDelayed;
    int timeoutCount;
    clientSnapshot_t frames[0x20];
    int ping;
    int rate;
    int snapshotMsec;
    int pureAuthentic;
    netchan_t netchan;
    byte netProfiling[0x38000];
    int guid;
    uint16_t scriptId;
    int bIsTestClient;
    int serverId;
    byte voicePackets[261][0x28];
    int voicePacketCount;
    bool muteList[0x40];
    bool sendVoice;
    char PBguid[0x21];
    char clientPBguid[0x21];
} client_t;
static_assert((sizeof(client_t) == 0xb1064));



#define SV_CMD_CAN_IGNORE 0
#define SV_CMD_RELIABLE 1

/** Sends a command string to a client. If clientNum is -1, then the command is sent to everybody */
inline void SV_GameSendServerCommand( int clientNum, int svscmd_type, const char *text ) {
    WL(
        ASM_CALL(RETURN_VOID, 0x004567b0, 0, EAX(clientNum), EDX(svscmd_type), ECX(text)),
        ASM_CALL(RETURN_VOID, 0x080917aa, 3, PUSH(clientNum), PUSH(svscmd_type), PUSH(text))
    )
};

// Set cvar on client side
inline void SV_SetClientCvar(int clientNum, const char *cvarName, const char *cvarValue) {
    SV_GameSendServerCommand(clientNum, SV_CMD_RELIABLE, va("%c %s \"%s\"", 118, cvarName, cvarValue));
}


// Get user info
inline void SV_GetUserInfo(int index, char *buffer, int bufferSize) {
    WL(
        ASM_CALL(RETURN_VOID, 0x004580b0, 0, EAX(index), EBX(buffer), EDI(bufferSize)),
        ASM_CALL(RETURN_VOID, 0x08092C04, 3, PUSH(index), PUSH(buffer), PUSH(bufferSize))
    )
}

// Kick player from the server, returns guid
inline int SV_KickClient(void* client) {
    const char* playerName = NULL;
    int playerNameLen = 0;
    int result;
    WL(
        ASM_CALL(RETURN(result), 0x004521b0, 0, EDI(client), EAX(playerName), EBX(playerNameLen)),
        ASM_CALL(RETURN(result), 0x0808C316, 3, PUSH(client), PUSH(playerName), PUSH(playerNameLen))
    );
    return result;
}

// Kick player from the server with message
inline void SV_DropClient(client_t* client, char const* reason) {
    WL(
        ASM_CALL(RETURN_VOID, 0x00454750, 1, EAX(client), PUSH(reason)),
        ASM_CALL(RETURN_VOID, 0x0808f02e, 2, PUSH(client), PUSH(reason))
    );
}

// Returns 1 if the map exists on the server
inline int SV_MapExists(const char* mapName)
{
    int ret;
    WL(
        ASM_CALL(RETURN(ret), 0x00457870, 0, ECX(mapName)),
        ASM_CALL(RETURN(ret), 0x08092302, 1, PUSH(mapName))
    );
    return ret;
}
#endif