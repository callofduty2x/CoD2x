/*
	CoD2x test mod - killfeed

	Tests the killfeed of the CoD2x client: the extended GSC function obituary and the cg_gameMessage* cvars.
		obituary(victim, attacker, sWeapon, sMeansOfDeath [, icon1, icon2, icon3, icon4])

	You spawn right away, bots are added and lined up in front of you. The test cases run one by one: a bot kills
	another bot (or itself, or you kill a bot) and the case calls obituary with its arguments. The case and the
	expected killfeed line are printed in the center of the screen and to the console.

	When you kill a bot yourself, the icons come from the real kill, as a mod would do it:
		wallbang  - the bullet went through a wall (sv_wallbang 1, flag 64 in iDFlags)
		no-scope  - sniper rifle without aiming down the sight
		headshot  - hit to the head

	The game has no wallbang / no-scope icons, stock materials are used as stand-ins (a real mod has its own):
		wallbang = hint_mantle, noscope = stance_stand, extra = objpoint_star, extra2 = headicon_dead

	Server cvars:
		scr_test_auto 1              run the cases one by one, 0 stops
		scr_test_interval 3          seconds between the cases
		scr_test_case <id>           run one case now, e.g. "scr_test_case e4", also the manual cases m* and x*
		                             (the x* cases end with a script error - fatal with developer_script 1)
		scr_test_list 1              print all cases to the console
		scr_test_bots 6              number of bots
		scr_test_lineup 1            line up the bots in front of you again
		scr_test_lineup_distance 256 distance of the bots
		scr_test_real_icons 1        icons for your own kills, 0 = vanilla obituary (as an old mod)
		scr_test_onehit 1            your hits kill with one shot
		scr_test_hint 1              print the case in the center of the screen

	A zPAM gametype: it runs on top of zPAM in the main folder (the mod itself is fs_game mods/test) and uses zPAM's systems
	(events, cvars, the code callbacks in _callbacksetup.gsc), but none of zPAM's game modules (readyup, menus, ...).
	zPAM's InitSystems() is replaced by initTestSystems() below, without the parts a test gametype does not want.
*/

#include maps\mp\gametypes\global\_global;

main()
{
	// zPAM systems: events, cvars, precache, hud
	initTestSystems();

	addEventListener("onStartGameType", ::onStartGameType);
	addEventListener("onConnecting", ::onConnecting);
	addEventListener("onPlayerDamaging", ::onPlayerDamaging);

	// Called by zPAM's _callbacksetup.gsc after the events
	level.onAfterConnected = ::onAfterConnected;
	level.onAfterPlayerDamaged = ::onAfterPlayerDamaged;
	level.onAfterPlayerKilled = ::onAfterPlayerKilled;
	level.spawnSpectator = ::spawnSpectator;	// when zPAM is not installed correctly

	level.iDFLAGS_WALLBANG = 64;	// CoD2x: the bullet went through a wall (sv_wallbang 1)

	// Only the zPAM modules the callbacks need, not InitModules()
	maps\mp\gametypes\_bots::Init();			// adding bots, pers["isBot"]
	maps\mp\gametypes\_weapons::init();			// cvars of the damage callback (hitbox fixes, ...)
	maps\mp\gametypes\_damagefeedback::init();	// hit marker of the damage callback
}

// zPAM's global\_init.gsc::InitSystems() with two changes:
//  - no rules::Init(): it loads the rules of zPAM's gametypes (sd, dm, ...) only and asserts for any other
//  - no installation check: pam::Init() adds CheckInstallation, it fails for a main folder with old zPAM iwds
//    and spawns everyone outside the map - not what a test is about
// (GSC names are case-insensitive, initSystems would clash with InitSystems of _global.gsc)
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
	setCvar("g_debugDamage", 1);	// zPAM forces 0, the damage callback prints every hit with it
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
		testLog("^1test_killfeed: the map has no mp_dm_spawn spawnpoints");
		exitLevel(false);
		return;
	}
	for(i = 0; i < level.spawnpoints.size; i++)
		level.spawnpoints[i] placeSpawnpoint();

	allowed[0] = "dm";
	maps\mp\gametypes\_gameobjects::main(allowed);

	mptype\american_normandy::precache();
	mptype\german_normandy::precache();
	game["allies_model"] = mptype\american_normandy::main;
	game["axis_model"] = mptype\german_normandy::main;

	setDefaultCvar("scr_test_auto", "1");
	setDefaultCvar("scr_test_interval", "3");
	setDefaultCvar("scr_test_case", "");
	setDefaultCvar("scr_test_list", "0");
	setDefaultCvar("scr_test_bots", "6");
	setDefaultCvar("scr_test_lineup", "0");
	setDefaultCvar("scr_test_lineup_distance", "256");
	setDefaultCvar("scr_test_real_icons", "1");
	setDefaultCvar("scr_test_onehit", "1");
	setDefaultCvar("scr_test_hint", "1");

	initIcons();
	initWeapons();
	initCases();

	level.botTurn = 0;

	level thread addBots();
	level thread caseLoop();
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
* Icons, weapons and the test cases
****************************************************************************************************************/

initIcons()
{
	level.testIcon = [];
	addIcon("headshot", "killiconheadshot");
	addIcon("melee", "killiconmelee");
	addIcon("wallbang", "hint_mantle");				// stand-in
	addIcon("noscope", "stance_stand");				// stand-in
	addIcon("extra", "objpoint_star");
	addIcon("extra2", "headicon_dead");
	addIcon("kar98", "gfx/icons/hud@kar98.tga");	// the kill icon of kar98k_mp
}

addIcon(key, shader)
{
	level.testIcon[key] = shader;
	precacheShader(shader);
}

initWeapons()
{
	precacheItem("kar98k_mp");					// wide icon
	precacheItem("kar98k_sniper_mp");
	precacheItem("m1garand_mp");
	precacheItem("springfield_mp");
	precacheItem("thompson_mp");
	precacheItem("colt_mp");					// square icon
	precacheItem("frag_grenade_american_mp");	// square icon
	precacheItem("frag_grenade_german_mp");
	precacheItem("smoke_grenade_american_mp");	// no kill icon
}

/*
	Icons of a case, separated by space:
		name of level.testIcon, or a shader name as is
		-   undefined
		_   empty string ""
		#5  integer 5 (converted to "5", script error - not precached)

	Attacker of a case:
		bot         a bot kills another bot
		you         you kill a bot
		you_victim  a bot kills you
		self        a bot kills itself
		world       a bot dies without an attacker (falling)

	Expected line: [weapon] is the kill icon of the weapon, [name] an icon of level.testIcon.
*/
initCases()
{
	level.cases = [];

	// Vanilla - no icons, the killfeed is the same as with an old mod
	addCase("v1", true, "bot", "kar98k_mp", "MOD_RIFLE_BULLET", "torso_upper", "",
		"Vanilla: rifle", "[kar98k]");
	addCase("v2", true, "bot", "kar98k_mp", "MOD_HEAD_SHOT", "head", "",
		"Vanilla: headshot", "[headshot] only, no weapon");
	addCase("v3", true, "bot", "kar98k_mp", "MOD_MELEE", "torso_upper", "",
		"Vanilla: melee", "[melee] only, no weapon");
	addCase("v4", true, "bot", "colt_mp", "MOD_PISTOL_BULLET", "torso_upper", "",
		"Vanilla: pistol", "[colt] square");
	addCase("v5", true, "bot", "frag_grenade_german_mp", "MOD_GRENADE_SPLASH", "none", "",
		"Vanilla: grenade", "[grenade] square");
	addCase("v6", true, "self", "none", "MOD_SUICIDE", "none", "",
		"Vanilla: suicide", "victim only, [suicide]");
	addCase("v7", true, "world", "none", "MOD_FALLING", "none", "",
		"Vanilla: falling", "victim only, [falling]");
	addCase("v8", true, "bot", "kar98k_mp", "MOD_HEAD_SHOT", "head", "- _ -",
		"Only undefined and empty icons", "[headshot] only - same as vanilla");

	// Extended - the mod passes icons
	addCase("e1", true, "bot", "kar98k_mp", "MOD_HEAD_SHOT", "head", "headshot",
		"Headshot with the weapon", "[kar98k] [headshot]");
	addCase("e2", true, "bot", "kar98k_sniper_mp", "MOD_RIFLE_BULLET", "torso_upper", "noscope",
		"No-scope", "[scoped kar98k] [noscope]");
	addCase("e3", true, "bot", "m1garand_mp", "MOD_HEAD_SHOT", "head", "wallbang headshot",
		"Wallbang + headshot", "[m1garand] [wallbang] [headshot]");
	addCase("e4", true, "bot", "springfield_mp", "MOD_HEAD_SHOT", "head", "wallbang noscope headshot extra",
		"4 icons", "[springfield] [wallbang] [noscope] [headshot] [extra]");
	addCase("e5", true, "bot", "springfield_mp", "MOD_RIFLE_BULLET", "torso_upper", "extra wallbang noscope headshot extra2 melee",
		"6 icons - only the first 4", "[springfield] [extra] [wallbang] [noscope] [headshot]");
	addCase("e6", true, "bot", "kar98k_mp", "MOD_RIFLE_BULLET", "torso_upper", "- wallbang _ headshot",
		"Undefined and empty icons between", "[kar98k] [wallbang] [headshot]");
	addCase("e7", true, "bot", "kar98k_mp", "MOD_HEAD_SHOT", "head", "kar98 headshot",
		"Weapon icon passed as an icon", "[kar98k] [headshot] - the weapon icon is not doubled");
	addCase("e8", true, "bot", "kar98k_mp", "MOD_HEAD_SHOT", "head", "headshot headshot",
		"Same icon twice", "[kar98k] [headshot] [headshot]");
	addCase("e9", true, "bot", "kar98k_mp", "MOD_MELEE", "torso_upper", "melee",
		"Melee with the weapon", "[kar98k] [melee]");
	addCase("e10", true, "bot", "thompson_mp", "MOD_MELEE", "torso_upper", "wallbang",
		"Melee without the melee icon", "[thompson] [wallbang] - no melee icon");
	addCase("e11", true, "bot", "colt_mp", "MOD_HEAD_SHOT", "head", "headshot",
		"Pistol - square weapon icon", "[colt] square [headshot]");
	addCase("e12", true, "bot", "frag_grenade_american_mp", "MOD_GRENADE_SPLASH", "none", "wallbang",
		"Grenade - square weapon icon", "[grenade] square [wallbang]");
	addCase("e13", true, "bot", "smoke_grenade_american_mp", "MOD_GRENADE", "none", "extra",
		"Weapon without a kill icon", "the vanilla icon (as v-cases) [extra]");
	addCase("e14", true, "self", "frag_grenade_german_mp", "MOD_GRENADE_SPLASH", "none", "extra",
		"Suicide with an icon", "victim only, [grenade] [extra]");
	addCase("e15", true, "world", "none", "MOD_FALLING", "none", "extra",
		"Falling with an icon - no weapon", "victim only, [falling] [extra]");
	addCase("e16", true, "you", "kar98k_sniper_mp", "MOD_HEAD_SHOT", "head", "noscope headshot",
		"You as the attacker", "you [scoped kar98k] [noscope] [headshot] bot");
	c = addCase("e17", true, "bot", "kar98k_mp", "MOD_HEAD_SHOT", "head", "wallbang headshot",
		"Between normal game messages", "message, [kar98k] [wallbang] [headshot], message");
	c.special = "message";
	c = addCase("e18", true, "bot", "thompson_mp", "MOD_PISTOL_BULLET", "head", "wallbang noscope headshot extra",
		"All bots killed at once", "one line per bot, each with 4 icons");
	c.special = "burst";

	// Manual only (scr_test_case <id>)
	addCase("m1", false, "you_victim", "kar98k_mp", "MOD_HEAD_SHOT", "head", "wallbang headshot",
		"A bot kills you", "bot [kar98k] [wallbang] [headshot] you");
	addCase("x1", false, "bot", "kar98k_mp", "MOD_RIFLE_BULLET", "torso_upper", "objpoint_radio",
		"Error: icon not precached", "script error - shader not precached");
	addCase("x2", false, "bot", "kar98k_mp", "MOD_RIFLE_BULLET", "torso_upper", "#5",
		"Error: icon is a number", "script error - shader \"5\" not precached (the number is converted to a string)");
}

addCase(id, auto, attacker, weapon, mod, hitloc, icons, name, expect)
{
	c = spawnstruct();
	c.id = id;
	c.auto = auto;
	c.attacker = attacker;
	c.weapon = weapon;
	c.mod = mod;
	c.hitloc = hitloc;
	c.icons = icons;
	c.name = name;
	c.expect = expect;
	c.special = "";
	level.cases[level.cases.size] = c;
	return c;
}

findCase(id)
{
	for(i = 0; i < level.cases.size; i++)
	{
		if(level.cases[i].id == id)
			return level.cases[i];
	}
	return;
}

printCases()
{
	testLog("^3test_killfeed cases (scr_test_case <id>):");
	for(i = 0; i < level.cases.size; i++)
	{
		c = level.cases[i];
		auto = "";
		if(!c.auto)
			auto = " (manual)";
		testLog("  " + c.id + " - " + c.name + auto + ": " + c.weapon + " " + c.mod + " icons '" + c.icons + "' -> " + c.expect);
	}
}


/****************************************************************************************************************
* Running the cases
****************************************************************************************************************/

caseLoop()
{
	wait 3;

	if(level.pam_installation_error)
		testLog("^1test_killfeed: zPAM reports an installation error, players are spawned outside the map");

	printCases();

	next = 0;
	lastRun = 0;
	for(;;)
	{
		wait 0.25;

		if(getCvarInt("scr_test_list"))
		{
			setCvar("scr_test_list", "0");
			printCases();
		}

		if(getCvarInt("scr_test_lineup"))
		{
			setCvar("scr_test_lineup", "0");
			human = getHuman();
			if(isDefined(human))
				lineUpBots(human);
		}

		id = getCvar("scr_test_case");
		if(id != "")
		{
			setCvar("scr_test_case", "");
			c = findCase(id);
			if(isDefined(c))
				runCase(c);
			else
				testLog("^1test_killfeed: unknown case '" + id + "', see scr_test_list 1");
			lastRun = getTime();
			continue;
		}

		if(!getCvarInt("scr_test_auto"))
			continue;
		if(getTime() - lastRun < getCvarFloat("scr_test_interval") * 1000)
			continue;

		// Next automatic case
		c = level.cases[next];
		next = (next + 1) % level.cases.size;
		if(!c.auto)
			continue;

		runCase(c);
		lastRun = getTime();
	}
}

runCase(c)
{
	if(getCvarInt("scr_test_hint"))
	{
		iprintlnbold("^3" + c.id + " ^7" + c.name);
		iprintlnbold("^2" + c.expect);
	}
	testLog("^3test_killfeed " + c.id + "^7 " + c.name + " -> " + c.expect);

	if(c.special == "burst")
	{
		attacker = nextBot(undefined);
		bots = getAliveBots();
		killed = 0;
		for(i = 0; i < bots.size; i++)
		{
			if(isDefined(attacker) && bots[i] != attacker)
			{
				bots[i] killByCase(attacker, c);
				killed++;
			}
		}
		if(!killed)
			skipCase("not enough living bots");
		return;
	}

	victim = undefined;
	attacker = undefined;
	switch(c.attacker)
	{
		case "bot":
			victim = nextBot(undefined);
			attacker = nextBot(victim);
			break;
		case "you":
			attacker = getHuman();
			victim = nextBot(undefined);
			break;
		case "you_victim":
			victim = getHuman();
			attacker = nextBot(undefined);
			break;
		case "self":
			victim = nextBot(undefined);
			attacker = victim;
			break;
		case "world":
			victim = nextBot(undefined);
			break;
	}

	if(!isDefined(victim) || (!isDefined(attacker) && c.attacker != "world"))
	{
		skipCase("no living player for this case");
		return;
	}

	if(c.special == "message")
		iprintln("^5Normal game message before the kill");

	victim killByCase(attacker, c);

	if(c.special == "message")
		iprintln("^5Normal game message after the kill");
}

// Kills self through the engine, Callback_PlayerKilled then calls obituary of the case
killByCase(attacker, c)
{
	damageMod = c.mod;
	if(damageMod == "MOD_HEAD_SHOT")
		damageMod = "MOD_RIFLE_BULLET";

	attackerName = "world";
	if(isDefined(attacker))
		attackerName = attacker.name;
	testLog("  " + attackerName + "^7 kills " + self.name);

	self.testCase = c;
	self finishPlayerDamage(attacker, attacker, 1000, level.iDFLAGS_NO_KNOCKBACK, damageMod, c.weapon, self.origin, undefined, c.hitloc, 0);
	self.testCase = undefined;
}

skipCase(reason)
{
	iprintlnbold("^1Skipped - " + reason);
	testLog("  ^1skipped - " + reason);
}

// obituary with the icons of the case, the number of arguments is the number of icons
caseObituary(attacker, c)
{
	icons = [];
	if(c.icons != "")
		icons = strtok(c.icons, " ");

	switch(icons.size)
	{
		case 0:
			obituary(self, attacker, c.weapon, c.mod);
			break;
		case 1:
			obituary(self, attacker, c.weapon, c.mod, iconArg(icons[0]));
			break;
		case 2:
			obituary(self, attacker, c.weapon, c.mod, iconArg(icons[0]), iconArg(icons[1]));
			break;
		case 3:
			obituary(self, attacker, c.weapon, c.mod, iconArg(icons[0]), iconArg(icons[1]), iconArg(icons[2]));
			break;
		case 4:
			obituary(self, attacker, c.weapon, c.mod, iconArg(icons[0]), iconArg(icons[1]), iconArg(icons[2]), iconArg(icons[3]));
			break;
		case 5:
			obituary(self, attacker, c.weapon, c.mod, iconArg(icons[0]), iconArg(icons[1]), iconArg(icons[2]), iconArg(icons[3]), iconArg(icons[4]));
			break;
		default:
			obituary(self, attacker, c.weapon, c.mod, iconArg(icons[0]), iconArg(icons[1]), iconArg(icons[2]), iconArg(icons[3]), iconArg(icons[4]), iconArg(icons[5]));
			break;
	}
}

iconArg(token)
{
	if(token == "-")
		return;			// undefined
	if(token == "_")
		return "";
	if(token == "#5")
		return 5;
	if(isDefined(level.testIcon[token]))
		return level.testIcon[token];
	return token;
}

// obituary of a real kill, as a mod would do it
realObituary(attacker, sMeansOfDeath, sWeapon, sHitLoc)
{
	// Vanilla rule of the stock gametypes
	if(sHitLoc == "head" && sMeansOfDeath != "MOD_MELEE")
		sMeansOfDeath = "MOD_HEAD_SHOT";

	wallbang = undefined;
	noscope = undefined;
	headshot = undefined;

	if(isDefined(self.testDFlags) && (self.testDFlags & level.iDFLAGS_WALLBANG))
		wallbang = level.testIcon["wallbang"];

	ads = -1;
	if(isPlayer(attacker) && attacker != self && isSniper(sWeapon) && isBullet(sMeansOfDeath))
	{
		ads = attacker playerADS();
		if(ads < 1)
			noscope = level.testIcon["noscope"];
	}

	if(sMeansOfDeath == "MOD_HEAD_SHOT")
		headshot = level.testIcon["headshot"];

	dflags = "undefined";
	if(isDefined(self.testDFlags))
		dflags = self.testDFlags;
	testLog("^3test_killfeed real kill: " + sWeapon + " " + sMeansOfDeath + " hitloc " + sHitLoc + " iDFlags " + dflags + " ads " + ads);

	if(!getCvarInt("scr_test_real_icons"))
	{
		obituary(self, attacker, sWeapon, sMeansOfDeath);	// as an old mod
		return;
	}

	// Undefined icons are skipped, without any icon the killfeed is vanilla
	obituary(self, attacker, sWeapon, sMeansOfDeath, wallbang, noscope, headshot);
}

isSniper(weapon)
{
	switch(weapon)
	{
		case "kar98k_sniper_mp":
		case "springfield_mp":
		case "mosin_nagant_sniper_mp":
		case "enfield_scope_mp":
			return true;
	}
	return false;
}

isBullet(mod)
{
	return (mod == "MOD_RIFLE_BULLET" || mod == "MOD_PISTOL_BULLET" || mod == "MOD_HEAD_SHOT");
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
	if(getBots().size < wanted)
		testLog("^1test_killfeed: only " + getBots().size + " of " + wanted + " bots, raise sv_maxclients");

	// When you connected before the bots
	wait 0.5;
	human = getHuman();
	if(isDefined(human))
		lineUpBots(human);
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

getAliveBots()
{
	alive = [];
	bots = getBots();
	for(i = 0; i < bots.size; i++)
	{
		if(bots[i].sessionstate == "playing")
			alive[alive.size] = bots[i];
	}
	return alive;
}

// The bots take turns
nextBot(exclude)
{
	bots = getAliveBots();
	for(i = 0; i < bots.size; i++)
	{
		level.botTurn = (level.botTurn + 1) % bots.size;
		bot = bots[level.botTurn];
		if(!isDefined(exclude) || bot != exclude)
			return bot;
	}
	return;
}

getHuman()
{
	players = getentarray("player", "classname");
	for(i = 0; i < players.size; i++)
	{
		if(!isTestBot(players[i]) && players[i].sessionstate == "playing")
			return players[i];
	}
	return;
}

// Bots in a row in front of the player, facing the player. They respawn at the same place.
lineUpBots(player)
{
	bots = getBots();
	if(!bots.size)
		return;

	angles = player getPlayerAngles();
	forward = anglesToForward((0, angles[1], 0));
	right = anglesToRight((0, angles[1], 0));
	distance = getCvarFloat("scr_test_lineup_distance");

	// Not into a wall in front of the player
	eye = player.origin + (0, 0, 60);
	trace = bulletTrace(eye, eye + scaleVector(forward, distance), false, player);
	distance = distance * trace["fraction"] - 48;
	if(distance < 64)
		distance = 64;

	for(i = 0; i < bots.size; i++)
	{
		side = (i - (bots.size - 1) * 0.5) * 64;
		pos = player.origin + scaleVector(forward, distance) + scaleVector(right, side);
		trace = bulletTrace(pos + (0, 0, 64), pos - (0, 0, 1024), false, undefined);
		spot = trace["position"];
		toPlayer = vectorToAngles(player.origin - spot);

		bots[i].testSpot = spot;
		bots[i].testSpotAngles = (0, toPlayer[1], 0);

		if(bots[i].sessionstate == "playing")
		{
			bots[i] setOrigin(bots[i].testSpot);
			bots[i] setPlayerAngles(bots[i].testSpotAngles);
		}
	}
}

scaleVector(vec, scale)
{
	return (vec[0] * scale, vec[1] * scale, vec[2] * scale);
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

	if(!isDefined(self.pers["team"]))
	{
		if(isTestBot(self) && (self getEntityNumber() & 1))
			self.pers["team"] = "axis";
		else
			self.pers["team"] = "allies";
	}

	self spawnPlayer();

	if(!isTestBot(self))
	{
		wait 0.5;
		lineUpBots(self);
		self iprintlnbold("^3CoD2x killfeed test^7 - see the console, scr_test_list 1");
	}
}

// Return true to prevent the damage.
// Test clients move and shoot randomly - only the scripted kills and your shots count.
onPlayerDamaging(eInflictor, eAttacker, iDamage, iDFlags, sMeansOfDeath, sWeapon, vPoint, vDir, sHitLoc, psOffsetTime)
{
	return isPlayer(eAttacker) && isTestBot(eAttacker);
}

// zPAM's CodeCallback_PlayerDamage after its fixes and the events
onAfterPlayerDamaged(eInflictor, eAttacker, iDamage, iDFlags, sMeansOfDeath, sWeapon, vPoint, vDir, sHitLoc, psOffsetTime)
{
	if(self.sessionstate != "playing")
		return;

	if(iDamage < 1)
		iDamage = 1;

	if(getCvarInt("scr_test_onehit") && isPlayer(eAttacker) && eAttacker != self)
		iDamage = 1000;

	// Read by realObituary (wallbang flag 64)
	self.testDFlags = iDFlags;

	self finishPlayerDamage(eInflictor, eAttacker, iDamage, iDFlags, sMeansOfDeath, sWeapon, vPoint, vDir, sHitLoc, psOffsetTime);
}

// zPAM's CodeCallback_PlayerKilled after the events (sMeansOfDeath is already MOD_HEAD_SHOT for a headshot)
onAfterPlayerKilled(eInflictor, attacker, iDamage, sMeansOfDeath, sWeapon, vDir, sHitLoc, psOffsetTime, deathAnimDuration)
{
	self endon("disconnect");
	self endon("spawned");
	self notify("killed_player");

	if(self.sessionstate != "playing")
		return;

	if(!isDefined(attacker))
		attacker = self;

	if(isDefined(self.testCase))
		self caseObituary(attacker, self.testCase);
	else
		self realObituary(attacker, sMeansOfDeath, sWeapon, sHitLoc);

	self.sessionstate = "dead";
	self.statusicon = "hud_status_dead";

	// You respawn where you died, the bots at their place in the row
	if(!isTestBot(self))
	{
		self.testSpot = self.origin;
		self.testSpotAngles = self getPlayerAngles();
	}

	body = self cloneplayer(deathAnimDuration);

	wait 1.5;

	self thread spawnPlayer();
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
	self.testDFlags = undefined;
	self.testCase = undefined;

	if(isDefined(self.testSpot))
		self spawn(self.testSpot, self.testSpotAngles);
	else
	{
		spawnpoint = level.spawnpoints[randomInt(level.spawnpoints.size)];
		self spawn(spawnpoint.origin, spawnpoint.angles);
	}

	self detachAll();
	if(self.pers["team"] == "axis")
		[[game["axis_model"] ]]();
	else
		[[game["allies_model"] ]]();

	if(isTestBot(self))
	{
		self giveWeapon("kar98k_mp");
		self setSpawnWeapon("kar98k_mp");
		self freezeControls(true);	// test clients move and shoot randomly
		return;
	}

	self giveWeapon("kar98k_sniper_mp");
	self giveMaxAmmo("kar98k_sniper_mp");
	self giveWeapon("m1garand_mp");		// 2 weapon slots, a pistol would take one of them
	self giveMaxAmmo("m1garand_mp");
	self giveWeapon("frag_grenade_american_mp");
	self setWeaponClipAmmo("frag_grenade_american_mp", 3);
	self setSpawnWeapon("kar98k_sniper_mp");

	self thread refillAmmo();

	waittillframeend;
	self notify("spawned_player");
}

// zPAM's CodeCallback_PlayerConnect uses it when zPAM is not installed correctly
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

refillAmmo()
{
	self endon("disconnect");
	self endon("spawned");
	self endon("killed_player");

	for(;;)
	{
		wait 2;
		weapon = self getCurrentWeapon();
		if(weapon != "none")
			self giveMaxAmmo(weapon);
	}
}
