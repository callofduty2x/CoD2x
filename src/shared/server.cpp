#include "server.h"

#include "shared.h"
#include "animation.h"
#include "cod2_common.h"
#include "cod2_dvars.h"
#include "cod2_cmd.h"
#include "cod2_net.h"
#include "cod2_server.h"
#include "cod2_file.h"
#if COD2X_WIN32
#include "../mss32/updater.h"
#endif
#if COD2X_LINUX
#include "../linux/updater.h"
#endif


#define svs_authorizeAddress 					(*((netaddr_s*)(ADDR(0x00d52770, 0x084400f0))))
#define svs_challenges 							(*((challenge_t (*)[MAX_CHALLENGES])(ADDR(0x00d3575c, 0x084230dc))))
#define svs_time 								(*((int*)(ADDR(0x00d35704, 0x08423084))))
#define svs_nextHeartbeatTime 					(*((int*)(ADDR(0x00d35754, 0x084230d4))))
#define svs_nextStatusResponseTime 				(*((int*)(ADDR(0x00d35758, 0x084230d8))))
#define svs_clients 							(*((client_t (*)[64])(ADDR(0x00d3570c, 0x0842308c))))
#define originalAuthorizeServerUrl 				((const char*)(ADDR(0x005a3c90, 0x08149afb)))

#define MAX_MASTER_SERVERS  3
dvar_t*		sv_master[MAX_MASTER_SERVERS];
netaddr_s	masterServerAddr[MAX_MASTER_SERVERS] = { {}, {}, {} };
dvar_t*		sv_cracked;
dvar_t*		showpacketstrings;
int 		nextIPTime = 0;


void SV_VoicePacket(netaddr_s from, msg_t *msg) { 
	WL(
		ASM_CALL(RETURN_VOID, 0x0045a750, 5, EAX(msg), PUSH_STRUCT(from, 5)),	// on Windows, the msg is passed in EAX
		ASM_CALL(RETURN_VOID, 0x08094b56, 6, PUSH_STRUCT(from, 5), PUSH(msg))
	);
}

void SVC_Status(netaddr_s from) {
	ASM_CALL(RETURN_VOID, ADDR(0x0045a880, 0x08094c84), 5, PUSH_STRUCT(from, 5));
}

void SVC_Info(netaddr_s from) {
	ASM_CALL(RETURN_VOID, ADDR(0x0045ae80, 0x0809537c), 5, PUSH_STRUCT(from, 5));
}

void SVC_RemoteCommand(netaddr_s from) {
	ASM_CALL(RETURN_VOID, ADDR(0x004b8ac0, 0x08097188), 5, PUSH_STRUCT(from, 5));
}

bool SV_IsBannedGuid(int guid) {
	int ret;
	ASM_CALL(RETURN(ret), ADDR(0x004531d0, 0x0808d630), 1, PUSH(guid));
	return ret;
}

bool SV_IsTempBannedGuid(int guid) {
	int ret;
	ASM_CALL(RETURN(ret), ADDR(0x00453160, 0x0808d5ac), WL(0, 1), WL(EDI, PUSH)(guid));
	return ret;
}






/*
=================
SV_UserinfoChanged

Pull specific info from a newly changed userinfo string
into a more C friendly form.
=================
*/
void SV_UserinfoChanged( client_t *cl )
{
	const char	*val;
	int		i;

	// name for C code
	Q_strncpyz( cl->name, Info_ValueForKey (cl->userinfo, "name"), sizeof(cl->name) );

	// rate command

	// if the client is on the same subnet as the server and we aren't running an
	// internet public server, assume they don't need a rate choke
	if ( Sys_IsLANAddress( cl->netchan.remoteAddress ) && dedicated->value.integer != 2 )
	{
		cl->rate = 99999;	// lans should not rate limit
	}
	else
	{
		val = Info_ValueForKey (cl->userinfo, "rate");
		if (strlen(val))
		{
			i = atoi(val);
			cl->rate = i;
			if (cl->rate < 1000)
			{
				cl->rate = 1000;
			}
			else if (cl->rate > 90000)
			{
				cl->rate = 90000;
			}
		}
		else
		{
			cl->rate = 5000;
		}
	}

	// snaps command
	val = Info_ValueForKey (cl->userinfo, "snaps");
	if (strlen(val))
	{
		i = atoi(val);
		if ( i < 1 )
		{
			i = 1;
		}
		// CoD2x: from 30 to 40
		else if ( i > 1000 )
		{
			i = 1000;
		}
		// CoD2x: end
		cl->snapshotMsec = 1000/i;
	}
	else
	{
		cl->snapshotMsec = 50;
	}

	// voice command
	val = Info_ValueForKey (cl->userinfo, "cl_voice");
	cl->sendVoice = atoi(val) > 0;
	if ( cl->rate < 5000 )
		cl->sendVoice = 0;

	// wwwdl command
	val = Info_ValueForKey (cl->userinfo, "cl_wwwDownload");
	cl->wwwOk = atoi(val) > 0;
}

void SV_UserinfoChanged_Win32() {
	client_t *cl;
	ASM( movr, cl, "esi" );
	SV_UserinfoChanged(cl);
}




void server_unbanAll_command() {
	// Remove file main/ban.txt
	bool ok = FS_Delete("ban.txt");
	if (ok) {
		Com_Printf("All bans removed\n");
	} else {
		Com_Printf("Error removing bans\n");
	}
}


void SV_DirectConnect(netaddr_s addr)
{
	int i;

    Com_DPrintf("SV_DirectConnect(%s)\n", NET_AdrToString(addr));

    const char *infoKeyValue = Cmd_Argv(1);

    char str[1024];
    strncpy(str, infoKeyValue, 1023);
    str[1023] = '\0';

    int32_t protocolNum = atol(Info_ValueForKey(str, "protocol"));

    if (protocolNum != 118)
    {   
        const char* msg;
        switch (protocolNum)
        {
            case 115: msg = "Your version is 1.0\nYou need version 1.3 and CoD2x"; break;
            case 116: msg = "Your version is 1.1\nYou need version 1.3 and CoD2x"; break;
            case 117: msg = "Your version is 1.2\nYou need version 1.3 and CoD2x"; break;
            default: msg = "Your CoD2 version is unknown"; break;
        }
        NET_OutOfBandPrint(NS_SERVER, addr, va("error\nEXE_SERVER_IS_DIFFERENT_VER\x15%s%s\n\x00", APP_VERSION "\n\n", msg));
        Com_DPrintf("    rejected connect from protocol version %i (should be %i)\n", protocolNum, 118);
        return;
    }

    int32_t cod2xNum = atol(Info_ValueForKey(str, "protocol_cod2x"));

    // CoD2x is not installed on 1.3 client
    if (cod2xNum == 0) {
        NET_OutOfBandPrint(NS_SERVER, addr, va(
            "error\nEXE_SERVER_IS_DIFFERENT_VER\x15%s\n\x00", 
            APP_VERSION "\n\n" "Download CoD2x at:\n" APP_URL ""));
        Com_DPrintf("    rejected connect from non-CoD2x version\n");
        return;
    }

    // CoD2x is installed but the version is different
    if (cod2xNum < APP_VERSION_PROTOCOL) { // Older client can not connect newer server
        const char* msg = va(
            "error\nEXE_SERVER_IS_DIFFERENT_VER\x15%s\n\x00", 
            APP_VERSION "\n\n" "Update CoD2x to version " APP_VERSION " or above");
        NET_OutOfBandPrint(NS_SERVER, addr, msg);
        Com_DPrintf("    rejected connect from CoD2x version %i (should be %i)\n", cod2xNum, APP_VERSION_PROTOCOL);
        return;
    }


	int hwid = atoi(Info_ValueForKey(str, "cl_hwid"));

	if (hwid == 0)
	{
		Com_Printf("rejected connection from client without HWID\n");
		NET_OutOfBandPrint(NS_SERVER, addr, "error\n\x15You have invalid HWID");
		return;
	}

	int challenge = atoi( Info_ValueForKey( str, "challenge" ) );

	// loopback and bot clients don't need to challenge
	if (!NET_IsLocalAddress(addr))
	{
		for (i = 0; i < MAX_CHALLENGES; i++)
		{
			if ( NET_CompareAdr( addr, svs_challenges[i].adr ) )
			{
				if (challenge == svs_challenges[i].challenge )
					break;
			}
		}
		if (i == MAX_CHALLENGES)
			return; // will be handled in original function again

		// CoD2x: change GUID to HWID
		svs_challenges[i].guid = hwid;
	}


	if (SV_IsBannedGuid(hwid))
	{
		Com_Printf("rejected connection from permanently banned HWID %i\n", hwid);
		NET_OutOfBandPrint( NS_SERVER, svs_challenges[i].adr, "error\n\x15You are permanently banned from this server" );
		memset( &svs_challenges[i], 0, sizeof( svs_challenges[i]));
		return;
	}

	if (SV_IsTempBannedGuid(hwid))
	{
		Com_Printf("rejected connection from temporarily banned HWID %i\n", hwid);
		NET_OutOfBandPrint( NS_SERVER, svs_challenges[i].adr, "error\n\x15You are temporarily banned from this server" );
		memset( &svs_challenges[i], 0, sizeof( svs_challenges[i]));
		return;
	}


    // Call the original function
    ((void (*)(netaddr_s))ADDR(0x00453c20, 0x0808e2aa))(addr);
}


// Resolve the master server address
netaddr_s * SV_MasterAddress(int i)
{
	if (masterServerAddr[i].type == NA_INIT || sv_master[i]->modified)
	{
		sv_master[i]->modified = false;

		Com_Printf("Resolving master server %s\n", sv_master[i]->value.string);
		if (NET_StringToAdr(sv_master[i]->value.string, &masterServerAddr[i]) == 0)
		{
			Com_Printf("Couldn't resolve address: %s\n", sv_master[i]->value.string);
			// Address type is set to NA_BAD, so it will not be resolved again
		}
		else
		{
			if (strstr(":", sv_master[i]->value.string) == 0)
			{
				masterServerAddr[i].port = BigShort(SERVER_MASTER_PORT);
			}
			Com_Printf( "%s resolved to %s\n", sv_master[i]->value.string, NET_AdrToString(masterServerAddr[i]));
		}
	}
	return &masterServerAddr[i];
}

/**
 * Check if the address belongs to a master server.
 * If masterServerUri is NULL, it will check all master servers.
 * If masterServerUri is not NULL, it will check only the master server with this URI.
 */
bool server_isAddressMasterServer(netaddr_s from, const char* masterServerUri = NULL)
{
	if (masterServerUri == NULL) {
		// Check all master servers
		for (int i = 0; i < MAX_MASTER_SERVERS; i++) {
			netaddr_s adr = *SV_MasterAddress(i);
			if (NET_CompareBaseAdr(from, adr)) {
				return true;
			}
		}
	} else {
		for (int i = 0; i < MAX_MASTER_SERVERS; i++) {
			if (strcmp(sv_master[i]->value.string, masterServerUri) == 0) {
				netaddr_s adr = *SV_MasterAddress(i);
				if (NET_CompareBaseAdr(from, adr)) {
					return true;
				}
			}
		}
	}
	return false;
}

#define	HEARTBEAT_MSEC	180000 // 3 minutes
#define	STATUS_MSEC		600000 // 10 minutes

void SV_MasterHeartbeat( const char *hbname )
{
	int i;

	// "dedicated 1" is for lan play, "dedicated 2" is for public play
	if ( !dedicated || dedicated->value.integer != 2 )
	{
		return;     // only dedicated servers send heartbeats
	}

	// It time to send a heartbeat to the master servers
	if ( svs_time >= svs_nextHeartbeatTime )
	{
		svs_nextHeartbeatTime = svs_time + HEARTBEAT_MSEC;

		// CoD2x: Send heartbeats to multiple master servers
		for (i = 0 ; i < MAX_MASTER_SERVERS; i++)
		{
			if (sv_master[i]->value.string[0] == '\0')
				continue;
			
			SV_MasterAddress(i); // Resolve the master server address in cause its not resolved yet or sv_master was modified

			if (masterServerAddr[i].type != NA_BAD)
			{
				Com_DPrintf( "Sending heartbeat to %s\n", sv_master[i]->value.string );
				NET_OutOfBandPrint( NS_SERVER, masterServerAddr[i], va("heartbeat %s\n", hbname));
			}
		}
		// CoD2x: End
	}

	// Its time to send a status response to the master servers
	if ( svs_time >= svs_nextStatusResponseTime )
	{
		svs_nextStatusResponseTime = svs_time + STATUS_MSEC;

		// CoD2x: Send status to multiple master servers
		for (i = 0 ; i < MAX_MASTER_SERVERS; i++)
		{
			if (sv_master[i]->value.string[0] == '\0')
				continue;

			SV_MasterAddress(i); // Resolve the master server address in cause its not resolved yet or sv_master was modified

			if (masterServerAddr[i].type != NA_BAD)
			{
				SVC_Status(masterServerAddr[i]);
			}
		}
		// CoD2x: End
	}

	// CoD2x: Ask for IP and port of this server
	if (svs_time >= nextIPTime && nextIPTime > 0) 
	{
		//nextIPTime = svs_time + 2000; // Try again after 2 seconds, unless response is received
		nextIPTime = 0;

		for (i = 0 ; i < MAX_MASTER_SERVERS; i++)
		{
			if (strcmp(sv_master[i]->value.string, SERVER_MASTER_URI) != 0) // find CoD2x master server
				continue;

			SV_MasterAddress(i); // Resolve the master server address in cause its not resolved yet or sv_master was modified

			if (masterServerAddr[i].type != NA_BAD)
			{
				NET_OutOfBandPrint(NS_SERVER, masterServerAddr[i], "getIp");
			}
		}
	}
	// CoD2x: End
}



/**
 * Process the response from authorization server.
 * When the client starts to connect to the server, the client will send CDKEY to the authorization server along with the MD5 hash of CDKEY.
 * The server asks the autorization server if the clients IP with this MD5-CDKEY has valid CDKEY.
 * Example responses from the authorization server:
 * 	`ipAuthorize 1058970440 accept KEY_IS_GOOD 822818 9e85a9484dab10748d089ebf2a47b5e8`
 * 	`ipAuthorize 530451529 deny INVALID_CDKEY 0 e4faec25a8bb0b913f0930b3937fada4`
 */
void SV_AuthorizeIpPacket( netaddr_s from )
{
	int challenge;
	int i;
	const char    *response;
	const char    *info;
	char ret[1024];

	if (NET_CompareBaseAdrSigned(&from, &svs_authorizeAddress ) != 0)
	{
		Com_Printf( "SV_AuthorizeIpPacket: not from authorize server\n" );
		return;
	}

	challenge = atoi(Cmd_Argv(1));

	// Find the challenge
	for (i = 0; i < MAX_CHALLENGES; i++)
	{
		if (svs_challenges[i].challenge == challenge )
			break;
	}
	if (i == MAX_CHALLENGES )
	{
		Com_Printf( "SV_AuthorizeIpPacket: challenge not found\n" );
		return;
	}

	// send a packet back to the original client
	svs_challenges[i].pingTime = svs_time;

	response = Cmd_Argv( 2 );
	info = Cmd_Argv( 3 );

    // CoD2x: Cracked server
	if (sv_cracked->value.boolean)
	{
		/*  !!!
		    This code below was commented because some players are not able to reach auth server for some reson.
			Their key never reach the auth server so the server will always receive "CLIENT_UNKNOWN_TO_AUTH"
		*/
		// Even if the server is cracked, wait for the clients to be validated by the authorization server
		// If the client has valid key-code, the authorization server will send "accept" response and we can atleast get the client's GUID
		/*if (Q_stricmp( response, "deny" ) == 0 && info && info[0] && (Q_stricmp(info, "CLIENT_UNKNOWN_TO_AUTH") == 0 || Q_stricmp(info, "BAD_CDKEY") == 0))
		{
			NET_OutOfBandPrint(NS_SERVER, svs_challenges[i].adr, "needcdkey"); // Awaiting key code authorization warning
			memset( &svs_challenges[i], 0, sizeof( svs_challenges[i]));
			return;
		}*/

		// Always accept the connection, even if the key-code is invalid
		response = "accept";
		info = "KEY_IS_GOOD";
	}
    // CoD2x: End

	if (Q_stricmp(response, "demo") == 0)
	{
		// they are a demo client trying to connect to a real server
		NET_OutOfBandPrint( NS_SERVER, svs_challenges[i].adr, "error\nEXE_ERR_NOT_A_DEMO_SERVER" );
		memset( &svs_challenges[i], 0, sizeof( svs_challenges[i]));
		return;
	}

	if (Q_stricmp(response, "accept") == 0)
	{
		// CoD2x: GUID from authorization server is replaced by HWID - guid is writed in SV_DirectConnect
		#if 0
		svs_challenges[i].guid = atoi(Cmd_Argv( 4 ));

		if (SV_IsBannedGuid(svs_challenges[i].guid) )
		{
			Com_Printf("rejected connection from permanently banned GUID %i\n", svs_challenges[i].guid);
			NET_OutOfBandPrint( NS_SERVER, svs_challenges[i].adr, "error\n\x15You are permanently banned from this server" );
			memset( &svs_challenges[i], 0, sizeof( svs_challenges[i]));
			return;
		}

		if (SV_IsTempBannedGuid(svs_challenges[i].guid) )
		{
			Com_Printf("rejected connection from temporarily banned GUID %i\n", svs_challenges[i].guid);
			NET_OutOfBandPrint( NS_SERVER, svs_challenges[i].adr, "error\n\x15You are temporarily banned from this server" );
			memset( &svs_challenges[i], 0, sizeof( svs_challenges[i]));
			return;
		}
		#endif

		if (!svs_challenges[i].connected)
		{
			NET_OutOfBandPrint(NS_SERVER, svs_challenges[i].adr, va("challengeResponse %i", svs_challenges[i].challenge));
			return;
		}

		return;
	}


	// authorization failed
	if (Q_stricmp( response, "deny" ) == 0)
	{
		if (!info || !info[0] )
			NET_OutOfBandPrint(NS_SERVER, svs_challenges[i].adr, "error\nEXE_ERR_CDKEY_IN_USE"); // even if the keycode is really in use, the auth server sends "INVALID_CDKEY" anyway, so this is not printed

		else if (Q_stricmp(info, "CLIENT_UNKNOWN_TO_AUTH") == 0 || Q_stricmp(info, "BAD_CDKEY") == 0)
			NET_OutOfBandPrint(NS_SERVER, svs_challenges[i].adr, "needcdkey"); // show "Awaiting key code authorization" warning on client

		// Authorization server does not differentiate between cracked key and key in use, it always sends "INVALID_CDKEY"
		else if (Q_stricmp(info, "INVALID_CDKEY") == 0)
			NET_OutOfBandPrint(NS_SERVER, svs_challenges[i].adr, "error\nEXE_ERR_CDKEY_IN_USE");

		else if (Q_stricmp(info, "BANNED_CDKEY") == 0)
			NET_OutOfBandPrint(NS_SERVER, svs_challenges[i].adr, "error\nEXE_ERR_BAD_CDKEY");
		
		memset( &svs_challenges[i], 0, sizeof( svs_challenges[i]));
		return;
	}


	// invalid response
	if (!info || !info[0] )
		NET_OutOfBandPrint(NS_SERVER, svs_challenges[i].adr, "error\nEXE_ERR_BAD_CDKEY");
	else
	{
		snprintf(ret, sizeof(ret), "error\n%s", info);
		NET_OutOfBandPrint(NS_SERVER, svs_challenges[i].adr, ret);
	}

	memset( &svs_challenges[i], 0, sizeof( svs_challenges[i]));
	return;
}


// Sends 'getIpAuthorize %i %i.%i.%i.%i "%s" %i PB "%s"' to the authorization server
void SV_AuthorizeRequest(netaddr_s from, int challenge, const char* PBHASH) {
	#if COD2X_WIN32
		ASM_CALL(RETURN_VOID, 0x00452c80, 5, ESI(challenge), EDI(PBHASH), PUSH_STRUCT(from, 5));
	#endif
	#if COD2X_LINUX
		((void (*)(netaddr_s, int, const char*))(0x0808cfc6))(from, challenge, PBHASH);
	#endif
}


void SV_GetChallenge( netaddr_s from )
{
	int i;
	int oldest;
	int oldestTime;
	challenge_t *challenge;

	oldest = 0;
	oldestTime = 0x7fffffff;

	// see if we already have a challenge for this ip
	challenge = &svs_challenges[0];
	for ( i = 0 ; i < MAX_CHALLENGES ; i++, challenge++ )
	{
		if ( !challenge->connected && NET_CompareAdr( from, challenge->adr ) )
		{
			break;
		}
		if ( challenge->time < oldestTime )
		{
			oldestTime = challenge->time;
			oldest = i;
		}
	}

	if ( i == MAX_CHALLENGES )
	{
		// this is the first time this client has asked for a challenge
		challenge = &svs_challenges[oldest];

		challenge->challenge = ( ( rand() << 16 ) ^ rand() ) ^ svs_time;
		challenge->adr = from;
		challenge->firstTime = svs_time;
		challenge->firstPing = 0;
		challenge->time = svs_time;
		challenge->connected = 0;
		i = oldest;
	}

	// if they are on a lan address, send the challengeResponse immediately
	if ( !net_lanauthorize->value.boolean && Sys_IsLANAddress(from) )
	{
		challenge->pingTime = svs_time;
		NET_OutOfBandPrint(NS_SERVER, from, va("challengeResponse %i", challenge->challenge));
		return;
	}

	// look up the authorize server's IP
	if ( !svs_authorizeAddress.ip[0] && svs_authorizeAddress.type != NA_BAD )
	{
		Com_Printf( "Resolving authorization server %s\n", SERVER_ACTIVISION_AUTHORIZE_URI );
		if ( !NET_StringToAdr( SERVER_ACTIVISION_AUTHORIZE_URI, &svs_authorizeAddress ) )
		{
			Com_Printf( "Couldn't resolve address\n" );
			return;
		}
		svs_authorizeAddress.port = BigShort( SERVER_ACTIVISION_AUTHORIZE_PORT );
		Com_Printf( "%s resolved to %i.%i.%i.%i:%i\n", SERVER_ACTIVISION_AUTHORIZE_URI,
		            svs_authorizeAddress.ip[0], svs_authorizeAddress.ip[1],
		            svs_authorizeAddress.ip[2], svs_authorizeAddress.ip[3],
		            BigShort( svs_authorizeAddress.port ) );
	}

	// CoD2x: 
	// Originally the players were allowed to join after 7 seconds if the master server was not asking for 20mins
	// Now if the authorization server is not responding, the player will be allowed to join after 5 seconds
	if ( svs_time - challenge->firstTime > 5000 )
	{
		bool isMasterServer = server_isAddressMasterServer(from);
		if (!isMasterServer)
		{
			Com_DPrintf( "authorize server timed out\n" );

			challenge->pingTime = svs_time;
			NET_OutOfBandPrint( NS_SERVER, challenge->adr, va("challengeResponse %i", challenge->challenge) );

			return;
		}
	}

	const char* PBHASH = NULL;
	if (Cmd_Argc() == 3) {
		PBHASH = Cmd_Argv( 2 );
		strncpy(challenge->clientPBguid, PBHASH, sizeof(challenge->clientPBguid));
		challenge->clientPBguid[sizeof(challenge->clientPBguid) - 1] = '\0';
	}

	// otherwise send their ip to the authorize server
	SV_AuthorizeRequest(from, challenge->challenge, PBHASH);
}


void server_get_address_info(char* buffer, size_t bufferSize, netaddr_s addr) {
	if (NET_CompareAdr(addr, svs_authorizeAddress))
		snprintf(buffer, bufferSize, "%s (authorize)", SERVER_ACTIVISION_AUTHORIZE_URI);
	else
	{
		for (int i = 0; i < MAX_MASTER_SERVERS; i++)
		{
			if (NET_CompareAdr(addr, masterServerAddr[i]))
			{
				snprintf(buffer, bufferSize, "%s (sv_master%i)", sv_master[i]->value.string, i + 1);
				break;
			}
		}
	}
}


void SV_ConnectionlessPacket( netaddr_s from, msg_t *msg )
{
	char* s;
	const char* c;

	MSG_BeginReading(msg);
	MSG_ReadLong(msg); // skip the -1 marker
	SV_Netchan_AddOOBProfilePacket(msg->cursize);
	s = MSG_ReadStringLine( msg );
	Cmd_TokenizeString( s );
	c = Cmd_Argv( 0 );

	if (sv_packet_info->value.boolean )
	{
		Com_Printf("SV packet %s : %s\n", NET_AdrToString(from), c);
	}

	// CoD2x: Debug connection-less packets
    if (showpacketstrings->value.boolean) {

		char buffer[1024];
		escape_string(buffer, 1024, s, msg->cursize - 4);
		char addr_to_str[256] = {0};
		server_get_address_info(addr_to_str, sizeof(addr_to_str), from);
		Com_Printf("SV_ConnectionlessPacket: %s %s %i %s\n  > '%s'\n\n", NET_AdrToString(from), get_netadrtype_name(from.type), msg->cursize, addr_to_str, buffer);
	}
	// CoD2x: End


	if (Q_stricmp( c, "v") == 0)
	{
		SV_VoicePacket( from, msg );
	}
	else if (Q_stricmp( c,"getstatus") == 0)
	{
		SVC_Status( from  );
	}
	else if (Q_stricmp( c,"getinfo") == 0)
	{
		SVC_Info( from );
	}
	else if (Q_stricmp( c,"getchallenge") == 0)
	{
		SV_GetChallenge( from );
	}
	else if (Q_stricmp( c,"connect") == 0)
	{
		SV_DirectConnect( from );
	}
	else if (Q_stricmp( c,"ipAuthorize") == 0)
	{
		SV_AuthorizeIpPacket( from );
	}
	else if (Q_stricmp( c, "rcon") == 0)
	{
		SVC_RemoteCommand( from );
	}
	// CoD2x: Auto-Updater
    else if (Q_stricmp(c, "updateResponse") == 0)
    {
		#if COD2X_WIN32
		updater_updatePacketResponse(from);
		#endif
		#if COD2X_LINUX
		updater_updatePacketResponse(from);
		#endif
    }
	// CoD2x: Master server getIp
	else if (Q_stricmp(c, "getIpResponse") == 0)
	{
		nextIPTime = 0; // Stop asking for IP
		if (server_isAddressMasterServer(from) && Cmd_Argc() == 2)
		{
			const char* ip = Cmd_Argv(1);
			Com_Printf("Server IP: %s\n", ip);
		}
	}
	// CoD2x: End
	else if (Q_stricmp( c,"disconnect") == 0)
	{
		// if a client starts up a local server, we may see some spurious
		// server disconnect messages when their new server sees our final
		// sequenced messages to the old client
	}
}






int Sys_SendPacket(uint32_t length, const void* data, netaddr_s addr) {
    int ret;
    ASM_CALL(RETURN(ret), ADDR(0x00466f50, 0x080d54a4), WL(5, 7), // on Windows, the length and data are passed in ECX and EAX
        WL(ECX, PUSH)(length), WL(EAX, PUSH)(data), PUSH_STRUCT(addr, 5));
    return ret;
}
void NET_SendLoopPacket(netsrc_e mode, int length, const void *data, netaddr_s to ) {
    ASM_CALL(RETURN_VOID, ADDR(0x00448800, 0x0806c722), WL(7, 8), 
        WL(EAX, PUSH)(mode), PUSH(length), PUSH(data), PUSH_STRUCT(to, 5));
}


int NET_SendPacket(netsrc_e sock, int length, const void *data, netaddr_s addr_to ) { 
	if (showpackets->value.boolean && *(int *)data == -1)
	{
		Com_Printf("[client %i] send packet %4i\n", 0, length);
	}

	// CoD2x: Debug connection-less packets
    if (showpacketstrings->value.boolean && *(int *)data == -1) {

        char buffer[1024];
		escape_string(buffer, 1024, (char*)data + 4, length - 4);
		char addr_to_str[256] = {0};
		server_get_address_info(addr_to_str, sizeof(addr_to_str), addr_to);
        Com_Printf("NET_SendPacket: %s %s %i %s\n  < '%s'\n\n", NET_AdrToString(addr_to), get_netadrtype_name(addr_to.type), length, addr_to_str, buffer);
    }
	// CoD2x: End

	if (addr_to.type == NA_LOOPBACK )
	{
		NET_SendLoopPacket(sock, length, data, addr_to);
		return 1;
	}

	if (addr_to.type == NA_INIT || addr_to.type == NA_BAD)
		return 0;

	return Sys_SendPacket( length, data, addr_to );
}

int NET_SendPacket_Win32(netsrc_e mode, netaddr_s to) { 
	int length;
    void* data;
    ASM( movr, length, "esi" );
    ASM( movr, data, "edi" );
    return NET_SendPacket(mode, length, data, to);
}
int NET_SendPacket_Linux(netsrc_e mode, uint32_t length, int32_t* data, netaddr_s to) { 
    return NET_SendPacket(mode, length, data, to);
}


// Function called when a client fully connects to the server, original function calls "begin" to gsc script
// Its called on client connection and on map_restart (even on soft restart on next round)
void SV_ClientBegin(int clientNum) {
    
    // Set client cvar g_cod2x
    // This will ensure that the same client side bug fixes are applied
    SV_SetClientCvar(clientNum, "g_cod2x", TOSTRING(APP_VERSION_PROTOCOL));

}

void SV_ClientBegin_Win32() {
    // Get num in eax
    int clientNum;
    ASM( movr, clientNum, "eax" );
    // Call the original function
    const void* original_func = (void*)0x004fe460;
    ASM( mov, "eax", clientNum );
    ASM( call, original_func );
    // Call our function
    SV_ClientBegin(clientNum);
}

void SV_ClientBegin_Linux(int clientNum) {
    // Call the original function
    ((void (*)(int))0x080f90ae)(clientNum);
    // Call our function
    SV_ClientBegin(clientNum);
}





// Called when the server is started via /map or /devmap, or /map_restart
void SV_SpawnServer(char* mapname) {

    // Fix animation time from crouch to stand
    animation_changeFix(true);

    // Call the original function
    ((void (*)(char* mapname))ADDR(0x00458a40, 0x08093520))(mapname);

	nextIPTime = svs_time + 4000; // Ask for IP and port of this server in 4 seconds
}



void G_RunFrame(int time) {
    // Call the original function
    ASM_CALL(RETURN_VOID, ADDR(0x004fd1b0, 0x0810a13a), WL(0, 1), WL(EAX, PUSH)(time));
}

void G_RunFrame_Win32() {
	int time;
	ASM( movr, time, "eax" );
	// Call the original function
	G_RunFrame(time);
}



// Called after all is initialized on game start
void server_init()
{
	sv_master[0] = Dvar_RegisterString("sv_master1", SERVER_ACTIVISION_MASTER_URI, 	(dvarFlags_e)(DVAR_ROM | DVAR_CHANGEABLE_RESET));
	sv_master[1] = Dvar_RegisterString("sv_master2", SERVER_MASTER_URI, 			(dvarFlags_e)(DVAR_ROM | DVAR_CHANGEABLE_RESET));
	sv_master[2] = Dvar_RegisterString("sv_master3", "", 							(dvarFlags_e)(DVAR_CHANGEABLE_RESET));

	sv_cracked = Dvar_RegisterBool("sv_cracked", false, (dvarFlags_e)(DVAR_CHANGEABLE_RESET));

	// If the original binary has been cracked by changing the authorize server URL, set sv_cracked to true to maintain the same behavior
	if (strncmp(originalAuthorizeServerUrl, SERVER_ACTIVISION_AUTHORIZE_URI, 26) != 0) {
		Dvar_SetBool(sv_cracked, true);
	}
	
	showpacketstrings = Dvar_RegisterBool("showPacketStrings", false, DVAR_CHANGEABLE_RESET);

	
    Cmd_AddCommand("unbanAll", server_unbanAll_command);
}




// Server side hooks
// The hooked functions are the same for both Windows and Linux
void server_patch()
{
    // Patch the protocol version check - now its being done in SV_DirectConnect
    patch_byte(ADDR(0x00453ce3, 0x0808e34d), 0xeb); // from '7467 je' to 'eb67 jmp'

    // Remove the Com_DPrintf("SV_DirectConnect()\n") call - now its being done in SV_DirectConnect
    patch_nop(ADDR(0x00453c87, 0x0808e2f7), 5);

    // Hook the SV_ConnectionlessPacket function
    patch_call(ADDR(0x0045bbc2, 0x08096126), (unsigned int)SV_ConnectionlessPacket);

	// Hook SV_MasterHeartbeat
	patch_call(ADDR(0x0045c8b2, 0x08096e03), (unsigned int)SV_MasterHeartbeat); // "COD-2"
	patch_call(ADDR(0x0045a18f, 0x08097049), (unsigned int)SV_MasterHeartbeat); // "flatline"


    patch_call(ADDR(0x00447ed7, 0x0806bb8f), (unsigned int)WL(NET_SendPacket_Win32, NET_SendPacket_Linux)); 
    patch_call(ADDR(0x00448117, 0x0806bdf8), (unsigned int)WL(NET_SendPacket_Win32, NET_SendPacket_Linux)); 
    patch_call(ADDR(0x004489fd, 0x0806c9e0), (unsigned int)WL(NET_SendPacket_Win32, NET_SendPacket_Linux)); 
    patch_call(ADDR(0x00448add, 0x0806cafa), (unsigned int)WL(NET_SendPacket_Win32, NET_SendPacket_Linux)); 
    patch_call(ADDR(0x00448be9, 0x0806cc31), (unsigned int)WL(NET_SendPacket_Win32, NET_SendPacket_Linux)); 
    patch_call(ADDR(0x00448ce9, 0x0806cd4f), (unsigned int)WL(NET_SendPacket_Win32, NET_SendPacket_Linux)); 
    patch_call(ADDR(0x0045ca4b, 0x08097708), (unsigned int)WL(NET_SendPacket_Win32, NET_SendPacket_Linux)); 
    #if COD2X_WIN32
        patch_call(0x0041296b, (unsigned int)NET_SendPacket_Win32); // CL_Netchan_SendOOBPacket
    #endif

    // Hook the SV_SpawnServer function
    patch_call(ADDR(0x00451c7f, 0x0808be22), (unsigned int)SV_SpawnServer); // map / devmap
    patch_call(ADDR(0x00451f1c, 0x0808befa), (unsigned int)SV_SpawnServer); // map_restart


    // Hook the SV_ClientBegin function
    patch_call(ADDR(0x00454d12, 0x0808f6ee), (unsigned int)ADDR(SV_ClientBegin_Win32, SV_ClientBegin_Linux));


	// Hook the G_RunFrame function
    patch_call(ADDR(0x0045c1ff, 0x08096765), (unsigned int)ADDR(G_RunFrame_Win32, G_RunFrame)); // SV_RunFrame
    #if COD2X_WIN32
        patch_call(0x00459239, (unsigned int)G_RunFrame_Win32); // SV_SpawnServer, because windows compiler unpack some functions...
    #endif


	// Hook the SV_UserInfoChanged function
	patch_call(ADDR(0x00454626, 0x0808eedb), (unsigned int)WL(SV_UserinfoChanged_Win32, SV_UserinfoChanged)); // SV_DirectConnect
	patch_call(ADDR(0x00455c32, 0x08090a36), (unsigned int)WL(SV_UserinfoChanged_Win32, SV_UserinfoChanged)); // SV_UpdateUserinfo_f



    // Fix "+smoke" bug
    // When player holds smoke or grenade button but its not available, the player will be able to shoot
    // The problem is that when holding the frag/smoke key, the server sends an EV_EMPTY_OFFHAND event every client frame. 
    // These events are buffered into an array of 4 items, overwriting any older events that were previously buffered. 
    // Since the server frame is slower than the client frame, with sv_fps set to 30, snaps to 30, and cl_maxpackets to 100, 
    // there are approximately 8 events the server might want to send to other players, but only 4 of them can actually 
    // be sent through the network.

    #if COD2X_WIN32
        // replace jmp to ret (5 bytes)
        // orig: 004f4f5f  e99cfeffff         jmp     PM_SendEmtpyOffhandEvent
        // new:  004f4f5f  c390909090         ret
        patch_copy(0x004f4f5f, (void*)"\xc3\x90\x90\x90\x90", 5); 
    #endif
    #if COD2X_LINUX
        patch_nop(0x080efe12, 5); // remove call to PM_SendEmtpyOffhandEvent
    #endif


	// Disable "Giving %s a 999 ping - !count:" message
	patch_nop(ADDR(0x0045bf59, 0x080964df), 5);

	// Disable "^3WARNING: Non-localized %s string is not allowed to have letters in it. Must be changed over to a localized string: %s"
	patch_nop(ADDR(0x00504990, 0x0811098e), 5);

	// Disable other messages printed in the console
	patch_nop(ADDR(0x004fc807, 0x081094f5), 5); // Disable "==== ShutdownGame ===="
	//patch_nop(ADDR(0x004fc2aa, 0x081090a6), 5); // Disable "------- Game Initialization -------"
	patch_nop(ADDR(0x004fc2bc, 0x081090ba), 5); // Disable "gamename: Call of Duty 2"
	patch_nop(ADDR(0x004fc2cb, 0x081090ce), 5);	// Disable "gamedate: Jun 23 2006"
}