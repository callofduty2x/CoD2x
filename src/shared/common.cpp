#include "common.h"

#include "shared.h"
#include "cod2_common.h"
#include "cod2_cmd.h"

#define MAX_CONSOLE_LINES 32
#define com_consoleLines ((char**)ADDR(0x00c26110, 0x081a21e0))
#define com_numConsoleLines (*(int*)ADDR(0x00c27280, 0x081a21d4))

char* common_commandLineOriginal = NULL;
char common_commandLine[1024] = {0};

#define MAX_STRING_TOKENS   512




/*
============
Cmd_TokenizeString
Parses the given string into command line tokens.
The text is copied to a seperate buffer and 0 characters
are inserted in the apropriate place, The argv array
will point into this temporary buffer.
============
*/
int Cmd_TokenizeStringInternal( const char *text_in, int max_tokens, char **argv, char *textOut )
{
	const char *str;

	// clear previous args
	int argc = 0;

	// CoD2x: save original pointer to detect start-of-string for http:// check
	const char *text_start = text_in;
	// CoD2x: End

	while ( 1 )
	{
		if ( argc == MAX_STRING_TOKENS )
		{
			return 0;			// this is usually something malicious
		}

		if ( !--max_tokens )
			break;

		while ( 1 )
		{
			// skip whitespace
			while ( *text_in && *text_in <= ' ' )
			{
				text_in++;
			}
			if ( !*text_in )
			{
				return argc;			// all tokens parsed
			}

			// skip // comments
			if ( text_in[0] == '/' && text_in[1] == '/' )
			{
				// CoD2x: lets us put 'http://' in commandlines
				if ( text_in == text_start || ( text_in > text_start && text_in[-1] != ':' ) )
				{
					return argc;			// all tokens parsed
				}
				// CoD2x: End
			}

			// skip /* */ comments
			if ( text_in[0] == '/' && text_in[1] =='*' )
			{
				while ( *text_in && ( text_in[0] != '*' || text_in[1] != '/' ) )
				{
					text_in++;
				}
				if ( !*text_in )
				{
					return argc;		// all tokens parsed
				}
				text_in += 2;
			}
			else
			{
				break;			// we are ready to parse a token
			}
		}

		// handle quoted strings
		// NOTE TTimo this doesn't handle \" escaping
		if ( *text_in == '"' )
		{
			argv[argc++] = textOut;

			for ( str = text_in + 1; *str && *str != '"'; ++str )
			{
				if ( *str == '\\' && str[1] == '"' )
					++str;

				*textOut++ = *str;
			}

			*textOut++ = 0;

			if ( !*str )
				return argc;

			text_in = str + 1;

			if ( !*text_in )
				return argc;

			if ( *text_in <= ' ' )
				++text_in;
		}
		else
		{
			// regular token
			argv[argc] = textOut;
			argc++;

			// skip until whitespace, quote, or command
			while ( *text_in > ' ' )
			{
				if ( text_in[0] == '"' )
				{
					break;
				}

				if ( text_in[0] == '/' && text_in[1] == '/' )
				{
					// CoD2x: lets us put 'http://' in commandlines
					if ( text_in == text_start || ( text_in > text_start && text_in[-1] != ':' ) )
					{
						break;
					}
					// CoD2x: End
				}

				// skip /* */ comments
				if ( text_in[0] == '/' && text_in[1] =='*' )
				{
					break;
				}

				*textOut++ = *text_in++;
			}

			*textOut++ = 0;

			if ( !*text_in )
				return argc;

			if ( *text_in <= ' ' )
				++text_in;
		}
	}

	if ( !*text_in )
		return argc;

	argv[argc] = textOut;

	while ( *text_in )
	{
		*textOut++ = *text_in++;
	}

	*textOut = 0;

	return argc + 1;
}

// 004210d0    int32_t Cmd_TokenizeStringInternal(char* text_in @ ecx, int32_t max_tokens, char** argv, char* text_out @ eax)
int Cmd_TokenizeStringInternal_Win32(int32_t max_tokens, char** argv) {
	char* text_in;
	char* text_out;
	ASM( movr, text_in, "ecx" );
	ASM( movr, text_out, "eax" );

	return Cmd_TokenizeStringInternal(text_in, max_tokens, argv, text_out);
}




void Com_ParseCommandLine( char *commandLine )
{
    bool inq = 0;
    com_numConsoleLines = 0;

    common_commandLineOriginal = commandLine;

    // CoD2x: Copy command line to local buffer
    if (strlen(commandLine) >= sizeof(common_commandLine)) {
        Com_Error(ERR_DROP, "Command line too long: %s", commandLine);
        return;
    }
    strncpy(common_commandLine, commandLine, sizeof(common_commandLine) - 1);
    common_commandLine[sizeof(common_commandLine) - 1] = '\0'; // Ensure null termination
    commandLine = common_commandLine;
    // CoD2x: End

    // CoD2x: Check if whole string is in quotes and remove them
    if (commandLine[0] == '"' && commandLine[strlen(commandLine) - 1] == '"') {
        commandLine++;
        commandLine[strlen(commandLine) - 1] = 0;
    }
    // CoD2x: End

	while ( *commandLine )
	{
        // CoD2x: Allow escaping quotes in string
        if (inq && *commandLine == '\\' && *(commandLine + 1) == '"') {
            commandLine += 2;
            continue;
        }
        // CoD2x: End

        // CoD2x: Break on + as in original
        if (inq && *commandLine == ' ' && *(commandLine + 1) == '+') {
            inq = false;
        }
        // CoD2x: End

		else if ( *commandLine == '"' )
		{
			inq = !inq;
		}

        // CoD2x: Loop thru the quote, ignore separators, break on + as in original
        else if (inq) {
            commandLine++;
            continue;
        }
        // CoD2x: End

		// look for a + seperating character
		// if commandLine came from a file, we might have real line seperators
		if ( *commandLine == '+' || *commandLine == '\n' || /*CoD2x: comma */ *commandLine == ',' )
		{
			if ( com_numConsoleLines == MAX_CONSOLE_LINES )
			{
				return;
			}

            inq = false;

            // CoD2x: dont add empty lines
            if (*(commandLine + 1) != '\0' && *(commandLine + 1) != '+' && *(commandLine + 1) != '\n' && *(commandLine + 1) != ',') { // CoD2x: end
                com_consoleLines[com_numConsoleLines] = commandLine + 1;
                com_numConsoleLines++;
            }
			*commandLine = 0; // terminate previous command
		}
		commandLine++;
	}

    // CoD2x: Debug
    #if DEBUG && 0
        for (int i = 0; i < com_numConsoleLines; i++) {
            Com_Printf("CMD: '%s'\n", com_consoleLines[i]);
        }
    #endif
    // CoD2x: End
}

void Com_ParseCommandLine_Win32() {
    char* commandLine;
    ASM( movr, commandLine, "eax" );
    Com_ParseCommandLine(commandLine);
}




void cmd_commandLine() {
    Com_Printf("Command line: \n");
    Com_Printf("%s\n", common_commandLineOriginal);
    for (int i = 0; i < com_numConsoleLines; i++) {
        if (i > 0) {
            Com_Printf("\n");
        }
        Com_Printf("  '%s'", com_consoleLines[i]);
    }
    if (com_numConsoleLines == 0) {
        Com_Printf("''");
    }
    Com_Printf("\n");
}



void common_printInfo() {
    Com_Printf("-----------------------------------\n");
    Com_Printf("CoD2x " APP_VERSION " loaded\n");
    Com_Printf("-----------------------------------\n");
}




/** Called once when hot-reloading is activated. */
void common_unload() {
    Cmd_RemoveCommand("cmdLine");
}



/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void common_init() {
    Cmd_AddCommand("cmdLine", cmd_commandLine);
}


// Server side hooks
// The hooked functions are the same for both Windows and Linux
void common_patch()
{
    // Print into console when the app is started -> "CoD2 MP 1.3 build win-x86 May  1 2006"
    patch_string_ptr(ADDR(0x00434467 + 1, 0x080620c6 + 4), __DATE__ " " __TIME__);          // originally win: "May  1 2006",  linux: "Jun 23 2006"
    patch_string_ptr(ADDR(0x0043446c + 1, 0x080620ce + 4), ADDR("win-x86", "linux-i386"));  // original
    patch_string_ptr(ADDR(0x00434471 + 1, 0x080620d6 + 4), APP_VERSION);                    // originally "1.3"
    patch_string_ptr(ADDR(0x00434476 + 1, 0x080620de + 4), "CoD2 MP");                      // original
    patch_string_ptr(ADDR(0x0043447b + 1, 0x08062235 + 3), "%s %s build %s %s\n");          // original


    // Value of cvar /version   ->   "CoD2 MP 1.3 build pc_1.3_1_1 Mon May 01 2006 05:05:43PM linux-i386"
    patch_string_ptr(ADDR(0x004346de + 1, 0x08051e1e + 4), __DATE__ " " __TIME__);          // originally "Mon May 01 2006 05:05:43PM"
    patch_string_ptr(ADDR(0x004346e3 + 1, 0x08051e26 + 4), "by eyza");                      // originally "pc_1.3_1_1"
    patch_string_ptr(ADDR(0x004346f7 + 1, 0x08062219 + 4), ADDR("win-x86", "linux-i386"));  // original
    patch_string_ptr(ADDR(0x00434701 + 1, 0x08062225 + 4), APP_VERSION);                    // originally "1.3"
    patch_string_ptr(ADDR(0x00434706 + 1, 0x0806222d + 4), "CoD2 MP");                      // original
    patch_string_ptr(ADDR(0x0043470b + 1, 0x08062235 + 3), "%s %s build %s %s");            // original


    // Hook Com_ParseCommandLine
    patch_call(ADDR(0x004344a8, 0x080620fd), (unsigned int)ADDR(Com_ParseCommandLine_Win32, Com_ParseCommandLine));


    // Hook Cmd_TokenizeStringInternal
    #if COD2X_WIN32
		int addresses[] = { 0x0042127c, 0x004017de, 0x00401827, 0x0040197a, 0x004019d5, 0x00401a70, 0x00401a8d, 0x004057f2, 0x0040e914, 0x0042121b, 0x0042123b, 0x0042125c, 0x004214f2, 0x004327f3, 0x004328a5, 0x0043c56a, 0x0043c5d8, 0x0043c825, 0x0043c89a, 0x00455d25, 0x00456570, 0x0045ab65, 0x0045b2ad, 0x0045b692 };	
		for (int addr : addresses) {
			patch_call(addr, (unsigned int)Cmd_TokenizeStringInternal_Win32);
		}
	#endif
    #if COD2X_LINUX
   		patch_call(0x08060623, (unsigned int)Cmd_TokenizeStringInternal);
   		patch_call(0x080605f3, (unsigned int)Cmd_TokenizeStringInternal);
    #endif

    // Fix the port negative number when formatting IP address
    patch_string_ptr(ADDR(0x00447733 + 1, 0x0806b238 + 4), "%i.%i.%i.%i:%hu");          // originally "%i.%i.%i.%i:%i"
    #if COD2X_WIN32
        patch_string_ptr(0x00412143 + 1, "%i.%i.%i.%i:%hu");                         // originally "%i.%i.%i.%i:%i"
    #endif

    // Value of cvar /shortversion   ->   "1.3"
    // Also visible in menu right bottom corner
    patch_string_ptr(ADDR(0x0043477c + 1, 0x08062281 + 4), APP_VERSION);             // originally "1.3"


    // Value of cvar /protocol   ->   "118"
    // Changing this cvar does not involve the protocol number used while connecting to server
    // It only affect server info when asking for /serverinfo or server notify master server
    #if COD2X_WIN32
        patch_byte(0x00459754 + 1, PROTOCOL_VERSION);
    #endif
    #if COD2X_LINUX
        patch_byte(0x08093b2c + 4, PROTOCOL_VERSION);
        patch_byte(0x08093b34 + 4, PROTOCOL_VERSION);
        patch_byte(0x08093b3c + 4, PROTOCOL_VERSION);
    #endif



    // Fix "iwd/sum name mismatch" error (especially for Windows when doing devmap) caused by too many IWD files
    // The problem is in function Info_SetValueForKey_Big, which uses 0x2000 for buffer size, but there is a check for 0x400
    patch_int32(ADDR(0x0044b14b + 1, 0x080b89ed + 1), 0x2000); // original: cmp eax, 0x400



    // Set min and def value of cvar /com_hunkMegs to 512
    patch_int32(ADDR(0x0042c6d6 + 1, 0x080ab9b3 + 4), 512); // minimim value, originally is 1
    patch_int32(ADDR(0x0042c6d1 + 1, 0x080ab9bb + 4), 512); // default value, originally is 160
}