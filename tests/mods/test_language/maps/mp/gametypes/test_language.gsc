/*
	CoD2x test mod - languages, translations and fonts

	How a mod shows text in the language of each player:

	1. Localized strings &"TEST_LANGUAGE_<REFERENCE>" - translated by the game of the player
	   The translations are in localizedstrings/test_language.str (the file name is the prefix of the reference).
	   The server sends only the reference, every client looks it up in its own .str files, so each player sees his
	   own language. All languages are in one file:
	       REFERENCE           HELLO
	       LANG_ENGLISH        "Hello!"
	       LANG_CZECH          "Ahoj!"
	   Vanilla CoD2 ignores the language of a LANG_ line and keeps the last one for everybody (in the example every
	   player would see "Ahoj!"). CoD2x keeps the line of the language of the player, LANG_ENGLISH when there is none.
	   Works for:
	       - hud elements: setText(&"..."), label = &"..."      (precacheString first, max 256 strings per map)
	       - iprintln / iprintlnbold / centerprint style messages: iprintln(&"...", arg1, ...)
	         &&1, &&2, ... in the translation are replaced by the arguments, a player entity gives his name
	         (no precache needed, the client translates the message)
	       - menus (text "@TEST_LANGUAGE_HELLO" in .menu files)

	2. Plain strings - the server must know the language: player getLanguage() (CoD2x)
	   Returns the game language of the player ("english", "czech", ...) or undefined (bots, clients without CoD2x).
	   For text built on the server (names, numbers, generated text) that can not be a localized string.

	3. Fonts - the bytes of a text are drawn by the font of the player
	   The game has no Unicode. A western install (english, french, german, italian, spanish) has fonts with the
	   cp1252 characters, polish and czech installs cp1250, russian cp1251. The same byte 0xE8 is e-grave for an english
	   player and c-caron for a czech one. Write the .str file in UTF-8: CoD2x converts it to the code page of the
	   player when it is loaded. Plain strings are sent as they are, keep them ASCII.

	Client cvars to try (or the binds of test_language.cfg):
		cl_fontSet cod4      CoD4 fonts (the files fonts/cod4/* of the CoD2x language IWD), applied by vid_restart
		cl_fontSet original  fonts of the game
		cl_language          read-only, your game language as the server sees it

	Server cvars:
		scr_test_bots 2              number of bots (their language is undefined)
		scr_test_print_interval 15   seconds between the plain-string prints, 0 = off
*/

#include maps\mp\gametypes\global\_global;

main()
{
	// zPAM systems: events, cvars, precache, hud
	initTestSystems();

	addEventListener("onStartGameType", ::onStartGameType);
	addEventListener("onConnecting", ::onConnecting);

	// Called by zPAM's _callbacksetup.gsc after the events
	level.onAfterConnected = ::onAfterConnected;
	level.spawnSpectator = ::spawnSpectator;	// when zPAM is not installed correctly

	// Only the zPAM modules the callbacks need, not InitModules()
	maps\mp\gametypes\_bots::Init();			// adding bots, pers["isBot"]
	maps\mp\gametypes\_weapons::init();			// cvars of the damage callback
	maps\mp\gametypes\_damagefeedback::init();	// hit marker of the damage callback
}

// The same as in test_killfeed.gsc: zPAM's InitSystems() without rules::Init() and the installation check
initTestSystems()
{
	thread maps\mp\gametypes\_callbacksetup::Init();

	game["firstInit"] = !isDefined(game["firstInit"]);
	level thread resetTestFirstInit();

	maps\mp\gametypes\global\events::Init();
	maps\mp\gametypes\global\cvar_system::Init();

	onStartGameType = level.events.onStartGameType;
	maps\mp\gametypes\global\pam::Init();
	level.events.onStartGameType = onStartGameType;	// drops pam::onStartGameType -> CheckInstallation

	// What rules::Init() defines, read by zPAM's hud
	game["rules_leagueString"] = &"CoD2x test";
	game["rules_formatString"] = &"";
	game["rules_stringColor"] = (0.9, 1, 1);
	game["leagueOptionsString"] = &"";
	if(game["firstInit"])
	{
		precacheString(game["rules_leagueString"]);
		precacheString(game["rules_formatString"]);
	}

	maps\mp\gametypes\global\cvars::Init();
	if(game["firstInit"])
		maps\mp\gametypes\global\precache::Init();
	maps\mp\gametypes\global\hud_system::Init();
	level thread maps\mp\gametypes\global\developer::init();
}

resetTestFirstInit()
{
	wait 0;
	game["firstInit"] = false;
}

onStartGameType()
{
	setClientNameMode("auto_change");

	// Deathmatch spawnpoints, every stock map has them
	level.spawnpoints = getentarray("mp_dm_spawn", "classname");
	if(!level.spawnpoints.size)
	{
		testLog("^1test_language: the map has no mp_dm_spawn spawnpoints");
		exitLevel(false);
		return;
	}
	for(i = 0; i < level.spawnpoints.size; i++)
		level.spawnpoints[i] placeSpawnpoint();

	allowed[0] = "dm";
	maps\mp\gametypes\_gameobjects::main(allowed);

	mptype\american_normandy::precache();
	game["allies_model"] = mptype\american_normandy::main;
	precacheItem("m1garand_mp");

	setDefaultCvar("scr_test_bots", "2");
	setDefaultCvar("scr_test_print_interval", "15");

	// Localized strings used by hud elements must be precached (they become config strings)
	precacheString(&"TEST_LANGUAGE_HELLO");
	precacheString(&"TEST_LANGUAGE_BOTS");
	precacheString(&"TEST_LANGUAGE_SAME");
	precacheString(&"TEST_LANGUAGE_ORDER");
	precacheString(&"TEST_LANGUAGE_ONLY_ENGLISH");
	precacheString(&"TEST_LANGUAGE_FONT_WESTERN");
	precacheString(&"TEST_LANGUAGE_FONT_LANGUAGE");

	initPlainTexts();

	level thread addBots();
	level thread printLoop();
}

setDefaultCvar(name, value)
{
	if(getCvar(name) == "")
		setCvar(name, value);
}

// To the console (developer_script 1) and to games_mp.log
testLog(text)
{
	println(text);
	logPrint(text + "\n");
}


/****************************************************************************************************************
* Plain strings in the language of the player (getLanguage)
****************************************************************************************************************/

// The texts are ASCII only: a plain string is drawn by the font of the player, so the characters of one language
// would be wrong for players of another (a czech c-caron is an e-grave with the english font).
initPlainTexts()
{
	level.plainText = [];

	addPlainText("english", "hello",   "Plain string from the server, picked for your language (english)");
	addPlainText("french",  "hello",   "Texte simple du serveur, choisi pour votre langue (francais)");
	addPlainText("german",  "hello",   "Einfacher Text vom Server, fuer deine Sprache gewaehlt (Deutsch)");
	addPlainText("czech",   "hello",   "Obycejny text ze serveru, vybrany pro tvuj jazyk (cestina)");
	addPlainText("polish",  "hello",   "Zwykly tekst z serwera, wybrany dla twojego jezyka (polski)");

	addPlainText("english", "unknown", "Your language is unknown (bot or client without CoD2x), english is used");
}

addPlainText(language, key, text)
{
	if(!isDefined(level.plainText[language]))
		level.plainText[language] = [];
	level.plainText[language][key] = text;
}

languageToString(language)
{
	if(!isDefined(language))
		return "undefined";
	return language;
}

// Text in the language of the player, english when it has no translation
plainText(player, key)
{
	language = player getLanguage();
	if(isDefined(language) && isDefined(level.plainText[language]) && isDefined(level.plainText[language][key]))
		return level.plainText[language][key];
	return level.plainText["english"][key];
}

printLoop()
{
	for(;;)
	{
		interval = getCvarFloat("scr_test_print_interval");
		if(interval <= 0)
		{
			wait 1;
			continue;
		}
		wait interval;

		players = getentarray("player", "classname");
		for(i = 0; i < players.size; i++)
		{
			player = players[i];
			language = player getLanguage();

			// Plain string, chosen on the server
			if(isDefined(language))
				player iprintln(plainText(player, "hello"));
			else if(!isTestBot(player))
				player iprintln(plainText(player, "unknown"));

			testLog("test_language: " + player.name + " language=" + languageToString(language));
		}
	}
}


/****************************************************************************************************************
* Hud elements with localized strings
****************************************************************************************************************/

createHud()
{
	self.testHud = [];

	y = 60;
	y = self addHudLine(&"TEST_LANGUAGE_HELLO",         "default", 1.2, y, (1, 1, 1));
	y = self addHudLine(&"TEST_LANGUAGE_ORDER",         "default", 1.0, y, (0.8, 0.9, 1));
	y = self addHudLine(&"TEST_LANGUAGE_ONLY_ENGLISH",  "default", 1.0, y, (0.8, 0.9, 1));
	y = self addHudLine(&"TEST_LANGUAGE_SAME",          "default", 1.0, y, (0.8, 0.9, 1));
	y = self addHudLine(&"TEST_LANGUAGE_FONT_WESTERN",  "default", 1.2, y, (1, 1, 0.6));
	y = self addHudLine(&"TEST_LANGUAGE_FONT_WESTERN",  "bigfixed", 0.8, y, (1, 1, 0.6));
	y = self addHudLine(&"TEST_LANGUAGE_FONT_WESTERN",  "smallfixed", 1.0, y, (1, 1, 0.6));
	y = self addHudLine(&"TEST_LANGUAGE_FONT_LANGUAGE", "default", 1.2, y, (1, 1, 0.6));

	// Label + number: the label is a localized string, the number is added by the client
	hud = newClientHudElem(self);
	hud.x = 20;
	hud.y = y + 6;
	hud.alignX = "left";
	hud.alignY = "top";
	hud.font = "default";
	hud.fontScale = 1.2;
	hud.label = &"TEST_LANGUAGE_BOTS";
	hud setValue(getBots().size);
	self.testHud[self.testHud.size] = hud;
	self.testHudBots = hud;
}

addHudLine(text, font, scale, y, color)
{
	hud = newClientHudElem(self);
	hud.x = 20;
	hud.y = y;
	hud.alignX = "left";
	hud.alignY = "top";
	hud.font = font;
	hud.fontScale = scale;
	hud.color = color;
	hud setText(text);
	self.testHud[self.testHud.size] = hud;
	return y + 22 * scale;
}

updateBotsHud()
{
	players = getentarray("player", "classname");
	for(i = 0; i < players.size; i++)
	{
		if(isDefined(players[i].testHudBots))
			players[i].testHudBots setValue(getBots().size);
	}
}


/****************************************************************************************************************
* Players and bots
****************************************************************************************************************/

addBots()
{
	// All bots have the same address, the engine rejects a bot that connects "too soon" after the previous one
	setCvar("sv_reconnectlimit", "0");

	wait 2;

	wanted = getCvarInt("scr_test_bots");
	for(tries = 0; getBots().size < wanted && tries < wanted * 3; tries++)
	{
		maps\mp\gametypes\_bots::addBot(true);	// true = no zPAM bot logic (team balance, ...)
		wait 0.2;
	}

	updateBotsHud();
}

getBots()
{
	bots = [];
	players = getentarray("player", "classname");
	for(i = 0; i < players.size; i++)
	{
		if(isTestBot(players[i]))
			bots[bots.size] = players[i];
	}
	return bots;
}

// pers["isBot"] is set by zPAM's _bots.gsc
isTestBot(player)
{
	return isDefined(player.pers["isBot"]) && player.pers["isBot"];
}

// Before "begin", zPAM's _force_download.gsc (not used) would set these, _callbacksetup.gsc reads them
onConnecting(firstTime)
{
	self.pers["modDownloaded"] = true;
	self.pers["downloadDisableResponse"] = false;
}

// zPAM's CodeCallback_PlayerConnect after "begin"
onAfterConnected()
{
	self endon("disconnect");

	wait 0.05;	// a bot is connected inside addtestclient, pers["isBot"] is set after it returns

	self.pers["team"] = "allies";
	self spawnPlayer();

	language = self getLanguage();
	testLog("test_language: " + self.name + " connected, getLanguage() = " + languageToString(language));

	if(isTestBot(self))
	{
		updateBotsHud();
		return;
	}

	self createHud();

	// Localized message with an argument: every player sees it in his language, &&1 is the name of the player
	iprintln(&"TEST_LANGUAGE_WELCOME", self);

	// Plain string chosen on the server for this player
	wait 1;
	if(isDefined(language))
		self iprintlnbold(plainText(self, "hello"));
	else
		self iprintlnbold(plainText(self, "unknown"));
}

spawnPlayer()
{
	self endon("disconnect");
	self notify("spawned");

	resettimeout();

	self.sessionteam = "none";
	self.sessionstate = "playing";
	self.spectatorclient = -1;
	self.archivetime = 0;
	self.psoffsettime = 0;
	self.statusicon = "";
	self.maxhealth = 100;
	self.health = self.maxhealth;

	spawnpoint = level.spawnpoints[randomInt(level.spawnpoints.size)];
	self spawn(spawnpoint.origin, spawnpoint.angles);

	self detachAll();
	[[game["allies_model"] ]]();

	if(isTestBot(self))
	{
		self freezeControls(true);	// test clients move and shoot randomly
		return;
	}

	self giveWeapon("m1garand_mp");
	self giveMaxAmmo("m1garand_mp");
	self setSpawnWeapon("m1garand_mp");

	waittillframeend;
	self notify("spawned_player");
}

spawnSpectator(origin, angles)
{
	self notify("spawned");

	self.sessionteam = "spectator";
	self.sessionstate = "spectator";
	self.spectatorclient = -1;
	self.archivetime = 0;

	if(!isDefined(origin))
	{
		spawnpoint = level.spawnpoints[randomInt(level.spawnpoints.size)];
		origin = spawnpoint.origin;
		angles = spawnpoint.angles;
	}
	self spawn(origin, angles);

	self notify("spawned_spectator");
}
