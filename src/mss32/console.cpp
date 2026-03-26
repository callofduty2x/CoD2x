#include "console.h"

#include <windows.h>
#include <stdio.h>

#include "shared.h"
#include "cod2_shared.h"


#define S_WCD_HWNDBUFFER_ADDR (*((HWND*)0x00D76AFC))


// 00468b10    uint32_t Conbuf_AppendText(char* msg @ ecx)
void Conbuf_AppendText() {
    const char* pMsg;
    ASM( movr, pMsg, "ecx" );

    //#define CONSOLE_BUFFER_SIZE     16384 //
    #define CONSOLE_BUFFER_SIZE     32767 // CoD2x - bigger console buffer

	// Static to avoid a ~64 KB stack allocation on every call.
	// Safe because Conbuf_AppendText is only ever called from the main thread.
	static char buffer[CONSOLE_BUFFER_SIZE * 2];
	char *b = buffer;
	const char *msg;
	int bufLen;
	int i = 0;
	static unsigned long s_totalChars;

	//
	// if the message is REALLY long, use just the last portion of it
	//
	if ( strlen( pMsg ) > CONSOLE_BUFFER_SIZE - 1 ) {
		msg = pMsg + strlen( pMsg ) - CONSOLE_BUFFER_SIZE + 1;
	} else
	{
		msg = pMsg;
	}

	//
	// copy into an intermediate buffer
	//
	while ( msg[i] && ( (size_t)( b - buffer ) < sizeof( buffer ) - 1 ) )
	{
		if ( msg[i] == '\n' && msg[i + 1] == '\r' ) {
			b[0] = '\r';
			b[1] = '\n';
			b += 2;
			i++;
		} else if ( msg[i] == '\r' )     {
			b[0] = '\r';
			b[1] = '\n';
			b += 2;
		} else if ( msg[i] == '\n' )     {
			b[0] = '\r';
			b[1] = '\n';
			b += 2;
		} else if ( Q_IsColorString( &msg[i] ) )   {
			i++;
		} else
		{
			*b = msg[i];
			b++;
		}
		i++;
	}
	*b = 0;
	bufLen = b - buffer;

	s_totalChars += bufLen;

    // CoD2x: disable deleting all text in console when full
	/*
	//
	// replace selection instead of appending if we're overflowing
	//
	if ( s_totalChars > CONSOLE_BUFFER_SIZE ) {
		SendMessageA( S_WCD_HWNDBUFFER_ADDR, EM_SETSEL, 0, -1 ); // select all
		s_totalChars = bufLen;
	} else {
		// NERVE - SMF - always append at the bottom of the textbox
		SendMessageA( S_WCD_HWNDBUFFER_ADDR, EM_SETSEL, 0xFFFF, 0xFFFF ); // deselect
	}

	//
	// put this text into the windows console
	//
	SendMessageA( S_WCD_HWNDBUFFER_ADDR, EM_LINESCROLL, 0, 0xffff );
	SendMessageA( S_WCD_HWNDBUFFER_ADDR, EM_SCROLLCARET, 0, 0 );
	*/
    // CoD2x: End

	// CoD2x: delete only half of the text in console when full
	// replace selection instead of appending if we're overflowing
	if (s_totalChars > CONSOLE_BUFFER_SIZE) {
		// Select characters from the beginning of the text box
		SendMessageA(S_WCD_HWNDBUFFER_ADDR, EM_SETSEL, 0, s_totalChars / 2);
		
		// Delete the selected characters
		SendMessageA(S_WCD_HWNDBUFFER_ADDR, EM_REPLACESEL, FALSE, (LPARAM)"");
		
		// Adjust the total character count
		s_totalChars -= s_totalChars / 2;
	}
	SendMessageA(S_WCD_HWNDBUFFER_ADDR, EM_SETSEL, 0xFFFF, 0xFFFF); // deselect

	SendMessageA( S_WCD_HWNDBUFFER_ADDR, EM_REPLACESEL, 0, (LPARAM) buffer );
    // CoD2x: End

    //InvalidateRect(S_WCD_HWNDBUFFER_ADDR, NULL, TRUE);
}


/**
 * Read the game console text from S_WCD_HWNDBUFFER_ADDR (a Win32 EDIT control).
 * The content is already CRLF-terminated by the EDIT control.
 * Returns the number of bytes written into buf.
 */
size_t console_getLogs(char* buf, size_t bufSize) {
    if (!buf || bufSize < 2) return 0;

    HWND hwndBuffer = S_WCD_HWNDBUFFER_ADDR;
    if (!hwndBuffer || !IsWindow(hwndBuffer)) {
        return (size_t)snprintf(buf, bufSize, "(console window unavailable)\r\n");
    }

    int textLen = GetWindowTextLengthA(hwndBuffer);
    if (textLen <= 0) {
        return (size_t)snprintf(buf, bufSize, "(no logs)\r\n");
    }

    // Clamp to what fits in the destination buffer (leave room for NUL)
    if (textLen >= (int)(bufSize - 1))
        textLen = (int)(bufSize - 2);

    int got = GetWindowTextA(hwndBuffer, buf, textLen + 1);
    if (got <= 0) {
        return (size_t)snprintf(buf, bufSize, "(failed to read console logs)\r\n");
    }

    buf[got] = '\0';
    return (size_t)got;
}


/** Called only once on game start after common initialisation. */
void console_init() {
}

/** Called before the entry point is called. Installs memory patches. */
void console_patch() {
    patch_call(0x00431de7, (unsigned int)Conbuf_AppendText);
    patch_call(0x0046579e, (unsigned int)Conbuf_AppendText);
    patch_call(0x004657a8, (unsigned int)Conbuf_AppendText);
    patch_call(0x004686e8, (unsigned int)Conbuf_AppendText);
}
