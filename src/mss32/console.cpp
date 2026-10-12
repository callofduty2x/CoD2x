#include "console.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "shared.h"
#include "cod2_shared.h"


#define S_WCD_HWNDBUFFER_ADDR (*((HWND*)0x00D76AFC))

// Crash-safe mirror of console text, independent from the EDIT control.
// Writer uses a sequence lock so readers can take a consistent snapshot
// without blocking (important in crash context).
// CONSOLE_MIRROR_CAPACITY is defined in console.h.
static char g_consoleMirror[CONSOLE_MIRROR_CAPACITY];
static size_t g_consoleMirrorLen = 0;
static volatile LONG g_consoleMirrorSeq = 0;

static void console_mirror_append(const char* text, size_t textLen) {
	if (!text || textLen == 0) {
		return;
	}

	// Begin write section (odd seq means writer active).
	InterlockedIncrement(&g_consoleMirrorSeq);

	// Keep only the newest chunk if input itself is larger than capacity.
	if (textLen >= CONSOLE_MIRROR_CAPACITY - 1) {
		text += textLen - (CONSOLE_MIRROR_CAPACITY - 1);
		textLen = CONSOLE_MIRROR_CAPACITY - 1;
		memcpy(g_consoleMirror, text, textLen);
		g_consoleMirrorLen = textLen;
		g_consoleMirror[g_consoleMirrorLen] = '\0';
		InterlockedIncrement(&g_consoleMirrorSeq);
		return;
	}

	// If needed, drop oldest bytes from the front.
	size_t freeBytes = (CONSOLE_MIRROR_CAPACITY - 1) - g_consoleMirrorLen;
	if (textLen > freeBytes) {
		size_t drop = textLen - freeBytes;
		if (drop >= g_consoleMirrorLen) {
			g_consoleMirrorLen = 0;
		} else {
			memmove(g_consoleMirror, g_consoleMirror + drop, g_consoleMirrorLen - drop);
			g_consoleMirrorLen -= drop;
		}
	}

	memcpy(g_consoleMirror + g_consoleMirrorLen, text, textLen);
	g_consoleMirrorLen += textLen;
	g_consoleMirror[g_consoleMirrorLen] = '\0';

	// End write section (even seq means stable snapshot available).
	InterlockedIncrement(&g_consoleMirrorSeq);
}

static size_t console_mirror_snapshot(char* out, size_t outSize) {
	if (!out || outSize < 2) {
		return 0;
	}

	for (int tries = 0; tries < 8; ++tries) {
		LONG seqBefore = g_consoleMirrorSeq;
		if (seqBefore & 1) {
			continue; // writer in progress
		}

		size_t len = g_consoleMirrorLen;
		size_t srcOffset = 0;
		if (len >= outSize) {
			// Buffer too small: keep the NEWEST bytes (drop oldest from the front) so the
			// most recent logs are never lost and the last print is not cut mid-line.
			srcOffset = len - (outSize - 1);
			len = outSize - 1;
		}

		if (len > 0) {
			memcpy(out, g_consoleMirror + srcOffset, len);
		}
		out[len] = '\0';

		LONG seqAfter = g_consoleMirrorSeq;
		if (seqBefore == seqAfter && !(seqAfter & 1)) {
			// If we dropped bytes from the front, skip the (possibly partial) first line.
			if (srcOffset > 0) {
				char* nl = (char*)memchr(out, '\n', len);
				if (nl) {
					size_t skip = (size_t)(nl - out) + 1;
					memmove(out, out + skip, len - skip);
					len -= skip;
					out[len] = '\0';
				}
			}
			return len;
		}
	}

	return 0;
}


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

	// Keep a crash-safe mirror so handler can fetch logs even when UI thread is stuck.
	if (bufLen > 0) {
		console_mirror_append(buffer, (size_t)bufLen);
	}

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
 * Read the game console text from the crash-safe mirror buffer.
 * Returns the number of bytes written into buf.
 */
size_t console_getLogs(char* buf, size_t bufSize) {
	if (!buf || bufSize < 2) {
		return 0;
	}

	// Mirror does not depend on the UI thread and is safe in crash context.
	size_t mirrored = console_mirror_snapshot(buf, bufSize);
	if (mirrored > 0) {
		return mirrored;
	}

	return (size_t)snprintf(buf, bufSize, "(no logs)\r\n");
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
