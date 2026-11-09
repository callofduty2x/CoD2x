/*
 * Languages and fonts - per-player translations of mod strings, the language of the player for the server, font sets.
 *
 * How the game works (CoD2 1.3):
 *   - Text is plain bytes. The fonts are bitmaps with one glyph per byte value (0x01 .. 0xff); which character a byte
 *     shows depends on the font. The fonts of a western install (english, french, german, italian, spanish, british)
 *     have the cp1252 characters, the russian install has its own fonts with cp1251, polish and czech with cp1250, the
 *     asian languages (korean, taiwanese, japanese, chinese) read two bytes per character (SEH_ReadCharFromString).
 *   - The fonts are registered by the main menu (assetGlobalDef: font, smallFont, bigFont, extraBigFont, boldFont,
 *     consoleFont) with RegisterFontHack, which takes "fonts/<language>/<name>" for russian, polish, korean, taiwanese,
 *     japanese, chinese, thai and czech, and the path from the menu for the other languages. The console font is
 *     registered in CL_InitRenderer the same way (without czech, a vanilla bug).
 *   - Translations are the .str files in the folder "localizedstrings" of all IWDs. Every file is loaded (SE_LoadLanguage), but
 *     CStringEdPackage::ParseLine does not check the language of a "LANG_xxx" line - every line overwrites the string,
 *     the last one wins (with loc_forceEnglish only LANG_ENGLISH is loaded). The game is fine with it, because its
 *     files have LANG_ENGLISH and the one language of the install. A mod can not have more languages in one file.
 *
 * What CoD2x changes:
 *   - .str files: only the LANG_ line of the language of the player is loaded (LANG_ENGLISH when the string has no
 *     translation to it). A mod can have all languages in one file, every player sees his own (SE_LoadFileData).
 *   - .str files in UTF-8 (with or without BOM): converted to the code page of the fonts of the player (cp1252 western,
 *     cp1250 czech / polish, cp1251 russian, ...), characters the code page does not have become '?'. Files in a code
 *     page (the game, older mods) are loaded as before.
 *   - cl_language (userinfo, read-only): name of the language of the texts (english with loc_forceEnglish), so the server knows the language of the player
 *     (GSC: player getLanguage()) and can send plain text in it.
 *   - cl_fontSet (enum): font set, "original" = the fonts of the game, "cod4" = CoD4 fonts (fonts/cod4/<font> of the
 *     CoD2x IWD). A language with its own fonts (russian, polish, czech, asian) uses fonts/cod4/<language>/<font>, so
 *     a set never shows a language in a font without its characters. A missing font falls back to the font of the
 *     game. The change is applied by vid_restart (done automatically).
 *   - English fallback: the game uses only the localized assets of the current language ("localized_<language>_*.iwd",
 *     any IWD with that name makes the language selectable). CoD2x adds the English localized assets behind them, so
 *     a language pack with only fonts (or a part of the translations) shows the rest in English.
 *   - CoD2x language IWD (main/localized_CoD2x_01.iwd, embedded): the .str files of the game with all languages
 *     and the fonts of the languages with own fonts. Its languages can be selected without their localized IWDs.
 *     "CoD2x" is not a language: vanilla ignores the IWD, CoD2x loads it as English localized assets, in front of the
 *     English localized IWDs of the game.
 *   - Locked localized IWDs: they are not checked by pure servers and some world textures exist only in them (barrels,
 *     crates, ...). Such a texture in a localized IWD is used only if it is the official one (CRC32 from
 *     language_textures.h), so a player can not replace it by his own localized IWD.
 */

#include "language.h"

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "shared.h"
#include "language_textures.h"
#include "drawing.h"
#include "../shared/cod2_client.h"
#include "../shared/cod2_cmd.h"
#include "../shared/cod2_common.h"
#include "../shared/cod2_dvars.h"
#include "../shared/cod2_file.h"
#include "../shared/cod2_shared.h"


dvar_t* cl_fontSet = NULL;
dvar_t* cl_language = NULL;

// Values of cl_fontSet, the name is the folder of the set in "fonts/" (0 = the fonts of the game, no folder)
#define FONT_SET_ORIGINAL 0
#define FONT_SET_COD4 1
static const char* font_setNames[] = { "original", "cod4", NULL };

static int language_fontSetApplied = FONT_SET_ORIGINAL; // cl_fontSet the fonts were registered with
static bool language_fontsRegistered = false;



// ======================================================================================================
// Engine
// ======================================================================================================

// Renderer function, copied by the engine into the table gfx_funcs (0x0068a1e8) after the renderer is loaded.
// Loads "fonts/..." file and its material, or returns the font registered with that name before
// ("$font", ... are the fonts of the main menu, see R_DuplicateFont). A missing file is a fatal error,
// at most 16 fonts (the game uses 15 with the duplicates and the dev fonts).
// 1000bd00 (gfx)    Font_s* R_RegisterFont(char* name, int32_t imageTrack)
typedef fontHandle_t* (__cdecl *R_RegisterFont_t)(const char* name, int imageTrack);
#define gfx_funcs_R_RegisterFont        (*((R_RegisterFont_t*)0x0068a2e0))

// Search path of the file system - an IWD or a folder, the first one with the file wins.
// Localized assets ("localized_<language>_*.iwd", folder "main/<language>") are used only for their language
// (FS_UseSearchPath, inlined in the Windows build: !fs_ignoreLocalized && (!localized || language == loc_language)).
struct searchpath_t {
    searchpath_t* next;
    void* iwd;              // iwd_t, NULL for a folder
    void* dir;              // directory_t, NULL for an IWD
    int32_t localized;      // localized assets
    int32_t language;       // language of the localized assets
};
#define fs_searchpaths                  (*((searchpath_t**)0x019a1b6c))

// File of an IWD (FS_LoadZipFile, one per entry of the zip directory, in its order)
struct fileInIwd_t {
    uint32_t pos;           // offset of the entry in the zip directory
    char* name;             // path, lowercase
    fileInIwd_t* next;      // next file of the hash chain
};

// IWD loaded by FS_LoadZipFile
struct iwd_t {
    char iwdFilename[256];  // OS path
    char iwdBasename[256];  // file name without ".iwd"
    char iwdGamename[256];  // "main", mod folder
    void* handle;           // unzFile
    int32_t checksum;
    int32_t pure_checksum;
    int32_t numfiles;
    int32_t field_310;
    int32_t hashSize;
    fileInIwd_t** hashTable;
    fileInIwd_t* buildBuffer; // numfiles entries
};
static_assert(offsetof(iwd_t, handle) == 0x300, "iwd_t");
static_assert(offsetof(iwd_t, numfiles) == 0x30c, "iwd_t");
static_assert(offsetof(iwd_t, buildBuffer) == 0x31c, "iwd_t");



// ======================================================================================================
// Languages
// ======================================================================================================

// Name of the game language ("english", "czech", ...)
const char* language_getName(int language)
{
    if (language < 0 || language >= LANGUAGE_COUNT)
        language = 0;
    return g_languages[language].name;
}

static const char* Language_GetCurrentName()
{
    return language_getName(loc_language ? loc_language->value.integer : 0);
}

// Language of the texts the player sees, for the server (cl_language): english with loc_forceEnglish (only the
// LANG_ENGLISH lines are loaded), else the game language
static const char* Language_GetTextName()
{
    if (loc_forceEnglish && loc_forceEnglish->value.boolean)
        return language_getName(0);
    return Language_GetCurrentName();
}

// Folder of the fonts of a language that has its own fonts (other characters than the western fonts), or NULL.
// The same languages as in RegisterFontHack, czech included.
static const char* Language_GetFontFolder(int language)
{
    switch (language)
    {
        case 6:  return "russian";
        case 7:  return "polish";
        case 8:  return "korean";
        case 9:  return "taiwanese";
        case 10: return "japanese";
        case 11: return "chinese";
        case 12: return "thai";
        case 14: return "czech";
        default: return NULL;
    }
}



// ======================================================================================================
// Translations (.str files)
// ======================================================================================================

// Line of a .str file: start, keyword and the language of a LANG_ line
struct strLangLine_t {
    char* keyword;      // "LANG_..." in the file buffer
    bool current;       // the language of the player
    bool english;
    bool same;          // the value is "#same" (= the english text)
};

#define STR_MAX_LANG_LINES 32

// Disables a line, the parser reads "//" as a comment (CStringEdPackage::ReadLine removes it, an empty line is skipped)
static void Language_DisableLine(char* keyword)
{
    keyword[0] = '/';
    keyword[1] = '/';
}

// Keeps one LANG_ line of a REFERENCE: the language of the player, else english, else all of them (vanilla).
static void Language_FilterReference(strLangLine_t* lines, int count)
{
    int keep = -1;
    for (int i = 0; i < count; i++) {
        if (lines[i].current && !lines[i].same) {
            keep = i;
            break;
        }
    }
    if (keep < 0) {
        for (int i = 0; i < count; i++) {
            if (lines[i].english) {
                keep = i;
                break;
            }
        }
    }
    if (keep < 0)
        return;

    for (int i = 0; i < count; i++) {
        if (i != keep)
            Language_DisableLine(lines[i].keyword);
    }
}

// Removes the LANG_ lines of other languages from a .str file, so ParseLine sets each string only once.
static void Language_FilterStrFile(char* buffer)
{
    if (!loc_language || (loc_forceEnglish && loc_forceEnglish->value.boolean))
        return; // the parser loads only LANG_ENGLISH

    const char* currentName = Language_GetCurrentName();
    size_t currentLen = strlen(currentName);

    strLangLine_t lines[STR_MAX_LANG_LINES];
    int count = 0;

    for (char* line = buffer; line && *line; ) {
        char* end = strchr(line, '\n');

        char* p = line;
        while (*p == ' ' || *p == '\t')
            p++;

        if (I_strnicmp(p, "LANG_", 5) == 0) {
            char* lang = p + 5;
            size_t langLen = 0;
            while (lang[langLen] && lang[langLen] != ' ' && lang[langLen] != '\t' && lang[langLen] != '\r' && lang[langLen] != '\n')
                langLen++;

            const char* value = lang + langLen;
            while (*value == ' ' || *value == '\t')
                value++;

            if (count < STR_MAX_LANG_LINES) {
                strLangLine_t* l = &lines[count++];
                l->keyword = p;
                l->current = langLen == currentLen && I_strnicmp(lang, currentName, (int)langLen) == 0;
                l->english = langLen == 7 && I_strnicmp(lang, "english", 7) == 0;
                l->same = I_strnicmp(value, "\"#same\"", 7) == 0;
            }

        } else if (I_strnicmp(p, "REFERENCE", 9) == 0 || I_strnicmp(p, "ENDMARKER", 9) == 0) {
            Language_FilterReference(lines, count);
            count = 0;
        }

        line = end ? end + 1 : NULL;
    }

    Language_FilterReference(lines, count);
}

// Windows code page of the fonts of a language - the bytes the fonts of that language draw
static UINT Language_GetCodePage(int language)
{
    switch (language)
    {
        case 6:  return 1251;   // russian
        case 7:  return 1250;   // polish
        case 8:  return 949;    // korean (EUC-KR, two bytes per character)
        case 9:  return 950;    // taiwanese (Big5)
        case 10: return 932;    // japanese (Shift-JIS)
        case 11: return 936;    // chinese (GBK)
        case 12: return 874;    // thai (TIS-620)
        case 14: return 1250;   // czech
        default: return 1252;   // english, french, german, italian, spanish, british, leet
    }
}

// True if the file is UTF-8: it starts with the UTF-8 BOM, or it has non-ASCII bytes that are all valid UTF-8.
// The files of the game and of older mods are in the code page of their language (cp1252, ...), such text is
// practically never valid UTF-8 (an accented letter would have to be followed by exactly the right symbols).
static bool Language_IsUtf8(const char* text, int len, bool* hasBom)
{
    *hasBom = len >= 3 && (uint8_t)text[0] == 0xEF && (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF;
    if (*hasBom)
        return true;

    bool nonAscii = false;
    for (int i = 0; i < len && !nonAscii; i++)
        nonAscii = (uint8_t)text[i] >= 0x80;
    if (!nonAscii)
        return false; // ASCII only, the same in every code page

    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, len, NULL, 0) > 0;
}

// Converts a UTF-8 .str file to the code page of the fonts of the player, in place. A character the code page does not
// have becomes '?'. Every character takes the same or fewer bytes in the code page than in UTF-8 (1-2 bytes vs 2-4),
// so the text fits into the buffer. Returns the new length.
static int Language_ConvertUtf8StrFile(char* buffer, int len)
{
    bool hasBom;
    if (!Language_IsUtf8(buffer, len, &hasBom))
        return len;

    const char* text = hasBom ? buffer + 3 : buffer;
    int textLen = hasBom ? len - 3 : len;
    if (textLen <= 0) {
        buffer[0] = '\0';
        return 0;
    }

    int wideLen = MultiByteToWideChar(CP_UTF8, 0, text, textLen, NULL, 0);
    if (wideLen <= 0)
        return len;

    wchar_t* wide = (wchar_t*)malloc(wideLen * sizeof(wchar_t));
    char* converted = (char*)malloc(len + 1);
    int convertedLen = 0;
    if (wide && converted && MultiByteToWideChar(CP_UTF8, 0, text, textLen, wide, wideLen) == wideLen) {
        UINT codePage = Language_GetCodePage(loc_language ? loc_language->value.integer : 0);
        convertedLen = WideCharToMultiByte(codePage, 0, wide, wideLen, converted, len, "?", NULL);
    }

    if (convertedLen > 0 && convertedLen <= len) {
        memcpy(buffer, converted, convertedLen);
        buffer[convertedLen] = '\0';
        len = convertedLen;
    }

    free(wide);
    free(converted);
    return len;
}

// SE_LoadFileData (a function in the Mac build) is inlined in the Windows build: SE_Load calls FS_ReadFile directly.
// This hook replaces that call. Returns the length of the file, or -1.
// 004ad1b0    char* SE_Load(char* fileName @ esi, uint8_t forceEnglish)
static int SE_LoadFileData(const char* fileName, void** buffer)
{
    int len = FS_ReadFile(fileName, buffer);

    // CoD2x: only the language of the player is loaded from the file, a UTF-8 file is converted to his code page
    if (len > 0 && buffer && *buffer) {
        Language_FilterStrFile((char*)*buffer);
        len = Language_ConvertUtf8StrFile((char*)*buffer, len);
    }
    // CoD2x: End

    return len;
}



// ======================================================================================================
// English fallback of the localized assets
// ======================================================================================================

#define LANGUAGE_MAX_FALLBACK_PATHS 64

// English localized search paths marked with the current language (valid until the next FS_Startup)
static searchpath_t* language_fallbackPaths[LANGUAGE_MAX_FALLBACK_PATHS];
static int language_fallbackCount = 0;

// The CoD2x localized IWDs (see iwd.cpp, built outside the repository) - localized_CoD2x_01: the .str files of the
// game with all languages and cod2x.str (the strings of the CoD2x menus, source src/embedded/localized_CoD2x_01), the fonts of the languages with own fonts. The localized IWDs are not checked by pure
// servers, so it works on every server. CoD2x loads it as English localized assets (IwdFileLanguage), the fallback of
// every language, in front of the English IWDs of the game.
#define LANGUAGE_IWD_PREFIX "localized_CoD2x_"

// Languages the CoD2x language IWD has (translations, and fonts for the languages with own fonts) - selectable
// without the localized IWDs of that language
static const int language_cod2xLanguages[] = {
    1,  // french
    2,  // german
    3,  // italian
    4,  // spanish
    6,  // russian (fonts)
    7,  // polish (fonts)
    14, // czech (fonts)
};

static bool Language_IsCoD2xLanguage(int language)
{
    for (size_t i = 0; i < sizeof(language_cod2xLanguages) / sizeof(language_cod2xLanguages[0]); i++) {
        if (language_cod2xLanguages[i] == language)
            return true;
    }
    return false;
}

// True if the search path is a CoD2x localized IWD
static bool Language_IsCoD2xIwd(searchpath_t* sp)
{
    return sp->localized && sp->iwd && I_strnicmp(((iwd_t*)sp->iwd)->iwdBasename, LANGUAGE_IWD_PREFIX, (int)strlen(LANGUAGE_IWD_PREFIX)) == 0;
}

// True if the CoD2x language IWD is in the search path
static bool Language_IsCoD2xIwdLoaded()
{
    for (searchpath_t* sp = fs_searchpaths; sp; sp = sp->next) {
        if (Language_IsCoD2xIwd(sp))
            return true;
    }
    return false;
}

static bool Language_IsSearchPath(searchpath_t* path)
{
    for (searchpath_t* sp = fs_searchpaths; sp; sp = sp->next) {
        if (sp == path)
            return true;
    }
    return false;
}

// Gives the English fallback search paths back their language
static void Language_RestoreFallback()
{
    for (int i = 0; i < language_fallbackCount; i++) {
        if (Language_IsSearchPath(language_fallbackPaths[i]))
            language_fallbackPaths[i]->language = 0;
    }
    language_fallbackCount = 0;
}

// The English localized assets are moved to the end of the search path, the CoD2x IWDs first (IwdFileLanguage makes
// them English, iwdsort puts them behind the English IWDs of the game - their .str files must win).
// They also become the fallback of the current language: below the localized assets of the current language (which
// are at the end too) and marked with the current language, so the game uses them. What the language does not have
// (.str files of the game, sounds, ...) is then loaded in English instead of being missing. Without this a language
// pack without the game's .str files would show every menu text as the reference (or UNLOCALIZED(...) with loc_warnings).
static void Language_ApplyFallback()
{
    Language_RestoreFallback();

    int current = loc_language ? loc_language->value.integer : 0;

    // Only a language with own localized assets (its fonts) or from the CoD2x language IWD, else english would make
    // any language selectable (a language without its fonts)
    bool fallback = false;
    if (current != 0) {
        fallback = Language_IsCoD2xLanguage(current) && Language_IsCoD2xIwdLoaded();
        for (searchpath_t* sp = fs_searchpaths; sp && !fallback; sp = sp->next)
            fallback = sp->localized && sp->language == current;
    }

    // Take the english localized search paths out of the list, in their order, the CoD2x ones first
    searchpath_t* cod2x = NULL;
    searchpath_t** cod2xTail = &cod2x;
    searchpath_t* english = NULL;
    searchpath_t** englishTail = &english;
    for (searchpath_t** link = &fs_searchpaths; *link; ) {
        searchpath_t* sp = *link;
        if (sp->localized && sp->language == 0 && (!fallback || language_fallbackCount < LANGUAGE_MAX_FALLBACK_PATHS)) {
            *link = sp->next;
            sp->next = NULL;
            searchpath_t*** tail = Language_IsCoD2xIwd(sp) ? &cod2xTail : &englishTail;
            **tail = sp;
            *tail = &sp->next;

            if (fallback) {
                sp->language = current;
                language_fallbackPaths[language_fallbackCount++] = sp;
            }
        } else {
            link = &sp->next;
        }
    }
    // English: the English IWDs of the game first - an install with translated English IWDs (the czech version) keeps
    // its texts, as in vanilla. Other languages: the CoD2x IWDs first - their .str files have the language.
    searchpath_t* list;
    if (current == 0) {
        *englishTail = cod2x;
        list = english;
    } else {
        *cod2xTail = english;
        list = cod2x;
    }

    // Append them to the end
    searchpath_t** link = &fs_searchpaths;
    while (*link)
        link = &(*link)->next;
    *link = list;

    if (language_fallbackCount > 0)
        Com_DPrintf("English localized assets (%i) are the fallback of %s\n", language_fallbackCount, language_getName(current));
}

// Language of a localized IWD from its file name - the letters after "localized_" (iwdsort and
// FS_AddIwdFilesForGameDirectory replace "localized_" by spaces before, the first 10 characters are skipped).
// The result is a static buffer. Vanilla reads "CoD" from "localized_CoD2x_01" - not a language, the IWD is ignored.
// 00424650    char* IwdFileLanguage(char const* iwdFileName @ ebx)
static const char* IwdFileLanguage(const char* iwdFileName)
{
    // CoD2x: the CoD2x localized IWDs are English localized assets (the fallback of every language)
    if (strlen(iwdFileName) > 10 && I_strnicmp(iwdFileName + 10, "CoD2x_", 6) == 0)
        return "english";
    // CoD2x: End

    // Call the original function
    const char* ret;
    ASM_CALL(RETURN(ret), 0x00424650, 0, EBX(iwdFileName));
    return ret;
}

static const char* IwdFileLanguage_Win32()
{
    const char* iwdFileName;
    ASM( movr, iwdFileName, "ebx" );

    return IwdFileLanguage(iwdFileName);
}



// ======================================================================================================
// Locked localized IWDs
// ======================================================================================================

// Path of a file in an IWD for the comparison: lowercase, '/'
static void Language_NormalizePath(const char* path, char* out, size_t size)
{
    size_t i = 0;
    for (; path[i] && i + 1 < size; i++)
        out[i] = path[i] == '\\' ? '/' : (char)tolower((unsigned char)path[i]);
    out[i] = '\0';
}

// Index of the first row of the path in language_localizedTextures, or -1 (the table is sorted by path)
static int Language_FindLocalizedTexture(const char* path)
{
    int low = 0;
    int high = (int)(sizeof(language_localizedTextures) / sizeof(language_localizedTextures[0])) - 1;
    int found = -1;
    while (low <= high) {
        int mid = (low + high) / 2;
        int cmp = strcmp(language_localizedTextures[mid].path, path);
        if (cmp == 0)
            found = mid;
        if (cmp >= 0)
            high = mid - 1;
        else
            low = mid + 1;
    }
    return found;
}

// True if the CRC32 is an official one of the texture (rows from Language_FindLocalizedTexture)
static bool Language_IsOfficialTexture(int row, uint32_t crc32)
{
    int count = (int)(sizeof(language_localizedTextures) / sizeof(language_localizedTextures[0]));
    for (int i = row; i < count && strcmp(language_localizedTextures[i].path, language_localizedTextures[row].path) == 0; i++) {
        if (language_localizedTextures[i].crc32 == crc32)
            return true;
    }
    return false;
}

// Entry of a zip directory
struct languageZipEntry_t {
    std::string path;       // normalized
    uint32_t crc32;
};

// Reads the directory of a zip file (only the directory, no file is unpacked)
static bool Language_ReadZipDirectory(const char* filePath, std::vector<languageZipEntry_t>& entries)
{
    FILE* f = fopen(filePath, "rb");
    if (!f)
        return false;

    bool ok = false;
    do {
        // End of central directory: the last 22 bytes + a comment of max 64 KB
        if (fseek(f, 0, SEEK_END) != 0) break;
        long fileSize = ftell(f);
        if (fileSize < 22) break;
        long tailSize = fileSize < 22 + 0xFFFF ? fileSize : 22 + 0xFFFF;
        std::vector<uint8_t> tail(tailSize);
        if (fseek(f, fileSize - tailSize, SEEK_SET) != 0 || fread(tail.data(), 1, tailSize, f) != (size_t)tailSize) break;

        auto u16 = [](const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8)); };
        auto u32 = [](const uint8_t* p) { return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24)); };

        long eocd = -1;
        for (long i = tailSize - 22; i >= 0; i--) {
            if (u32(&tail[i]) == 0x06054b50) { eocd = i; break; }
        }
        if (eocd < 0) break;

        uint32_t count = u16(&tail[eocd + 10]);
        uint32_t dirSize = u32(&tail[eocd + 12]);
        uint32_t dirOffset = u32(&tail[eocd + 16]);
        if (dirSize > 16 * 1024 * 1024 || (long)dirOffset + (long)dirSize > fileSize) break;

        std::vector<uint8_t> dir(dirSize);
        if (fseek(f, dirOffset, SEEK_SET) != 0 || fread(dir.data(), 1, dirSize, f) != dirSize) break;

        size_t pos = 0;
        char path[256];
        for (uint32_t n = 0; n < count && pos + 46 <= dir.size(); n++) {
            const uint8_t* e = &dir[pos];
            if (u32(e) != 0x02014b50) break;
            uint32_t nameLen = u16(e + 28);
            if (pos + 46 + nameLen > dir.size()) break;
            std::string name((const char*)e + 46, nameLen);
            Language_NormalizePath(name.c_str(), path, sizeof(path));
            entries.push_back({ path, u32(e + 16) });
            pos += 46 + nameLen + u16(e + 30) + u16(e + 32);
        }
        ok = true;
    } while (0);

    fclose(f);
    return ok;
}

// Hides the files of a localized IWD that replace a world texture of the game with an unofficial one.
// A hidden file has an empty name: FS_FOpenFileRead and FS_ListFiles never match it, the game takes the file from the
// next search path (the official localized IWD).
static void Language_LockLocalizedIwd(iwd_t* iwd)
{
    std::vector<languageZipEntry_t> entries;
    bool directoryRead = false;
    int hidden = 0;
    char path[256];

    for (int i = 0; i < iwd->numfiles; i++) {
        fileInIwd_t* file = &iwd->buildBuffer[i];
        if (!file->name || !file->name[0])
            continue;
        Language_NormalizePath(file->name, path, sizeof(path));
        int row = Language_FindLocalizedTexture(path);
        if (row < 0)
            continue;

        // CRC32 from the zip directory (the IWD keeps only the names), its entries are in the same order
        if (!directoryRead) {
            directoryRead = true;
            Language_ReadZipDirectory(iwd->iwdFilename, entries);
        }
        const languageZipEntry_t* entry = NULL;
        if (i < (int)entries.size() && entries[i].path == path)
            entry = &entries[i];
        for (size_t j = 0; !entry && j < entries.size(); j++) {
            if (entries[j].path == path)
                entry = &entries[j];
        }

        if (entry && Language_IsOfficialTexture(row, entry->crc32))
            continue;

        file->name[0] = '\0';
        hidden++;
    }

    if (hidden > 0)
        Com_Printf("^3Localized IWD %s: %i files are not the official textures of the game and are not used\n", iwd->iwdBasename, hidden);
}

// A localized folder ("main/<language>", FS_AddGameDirectoryInternal) - its loose files are used also on pure servers.
// The game never has world textures there, a folder with one is not used at all (language -1, FS_UseSearchPath skips it).
static void Language_LockLocalizedFolder(searchpath_t* sp)
{
    const char* base = (const char*)sp->dir;            // directory_t: path[256], gamedir[256]
    const char* gamedir = (const char*)sp->dir + 0x100;
    int count = (int)(sizeof(language_localizedTextures) / sizeof(language_localizedTextures[0]));

    for (int i = 0; i < count; i++) {
        if (i > 0 && strcmp(language_localizedTextures[i].path, language_localizedTextures[i - 1].path) == 0)
            continue;
        char ospath[MAX_OSPATH];
        snprintf(ospath, sizeof(ospath), "%s/%s/%s", base, gamedir, language_localizedTextures[i].path);
        if (GetFileAttributesA(ospath) != INVALID_FILE_ATTRIBUTES) {
            sp->language = -1;
            Com_Printf("^3Localized folder %s/%s has world textures of the game (%s), the folder is not used\n", base, gamedir, language_localizedTextures[i].path);
            return;
        }
    }
}

// Locks the localized IWDs and folders in the search path (they are loaded again by every FS_Startup). All of them,
// also the CoD2x IWDs - their name is no proof, and they have no world textures.
static void Language_LockLocalizedIwds()
{
    for (searchpath_t* sp = fs_searchpaths; sp; sp = sp->next) {
        if (sp->localized && sp->iwd)
            Language_LockLocalizedIwd((iwd_t*)sp->iwd);
        else if (sp->localized && sp->dir)
            Language_LockLocalizedFolder(sp);
    }
}

// Builds the search path. Called by FS_InitFilesystem, FS_Restart and the restricted demo mode, before the .str files
// are loaded.
// 00425270    char* FS_Startup(char* gameName)
static char* FS_Startup(const char* gameName)
{
    // CoD2x: the search paths of the previous start were freed by FS_Shutdown
    language_fallbackCount = 0;
    // CoD2x: End

    // Call the original function
    char* ret;
    ASM_CALL(RETURN(ret), 0x00425270, 1, PUSH(gameName));

    // CoD2x: unofficial world textures in localized IWDs are not used; English localized assets as the fallback of the
    // current language
    Language_LockLocalizedIwds();
    Language_ApplyFallback();
    // CoD2x: End

    return ret;
}

// Names of the languages for the enum dvar loc_language (g_languages order), NULL terminated
static const char* language_enumNames[LANGUAGE_COUNT + 1];

// Registers loc_language, loc_forceEnglish, loc_translate, loc_warnings and loc_warningsAsErrors (loc_language as an int
// 0 - 14, Dvar_RegisterInt inlined) and sets loc_isMultiByteLanguage.
// 004ab110    int32_t SEH_InitLanguage()
static int SEH_InitLanguage()
{
    // CoD2x: loc_language as an enum with the names of the languages - "loc_language czech" works, the console shows
    // the list. The value is the same index (Dvar_GetInt, Dvar_SetIntByName and the code read the integer), and a
    // number still works ("seta loc_language 14", Dvar_StringToEnum). It must be the first registration - the int
    // registrations after it (here, vid_restart) do not change the type of a registered dvar (Dvar_Reregister changes
    // only a dvar created by +set / the config).
    // 0 is "auto" - the language of the localized IWDs of the install (Language_ResolveAuto), english texts with
    // loc_forceEnglish.
    for (int i = 0; i < LANGUAGE_COUNT; i++)
        language_enumNames[i] = g_languages[i].name;
    language_enumNames[0] = "auto";
    language_enumNames[LANGUAGE_COUNT] = NULL;
    Dvar_RegisterEnum("loc_language", language_enumNames, 0, (enum dvarFlags_e)(DVAR_ARCHIVE | DVAR_LATCH | DVAR_CHANGEABLE_RESET));
    // CoD2x: End

    // Call the original function
    int ret;
    ASM_CALL(RETURN(ret), 0x004ab110, 0);
    return ret;
}

static void Language_ResolveAuto();

// Sets which languages have localized assets (g_languages[].hasAssets, the languages of the options menu), then
// SEH_VerifyLanguageSelection loads the .str files of loc_language (SE_LoadLanguage), or of the first language that
// has assets. Called after the file system starts and by vid_restart (loc_language is latched, it changes there).
// 004ab1e0    int32_t SEH_UpdateLanguageInfo()
static int SEH_UpdateLanguageInfo()
{
    // CoD2x: "auto" is the language of the install; the fallback for the language, which may have changed
    // (vid_restart) - before the .str files are loaded
    Language_ResolveAuto();
    Language_ApplyFallback();
    // CoD2x: End

    // Call the original function
    int ret;
    ASM_CALL(RETURN(ret), 0x004ab1e0, 0);

    // CoD2x: the fallback search paths are marked with the current language, english has the assets too;
    // the languages of the CoD2x language IWD can be selected in the options menu
    if (language_fallbackCount > 0)
        g_languages[0].hasAssets = 1;
    if (Language_IsCoD2xIwdLoaded()) {
        for (size_t i = 0; i < sizeof(language_cod2xLanguages) / sizeof(language_cod2xLanguages[0]); i++)
            g_languages[language_cod2xLanguages[i]].hasAssets = 1;
    }
    // CoD2x: End

    return ret;
}


// Language of the player. Vanilla chooses it in FS_InitFilesystem, before the config is executed: the default english,
// or the first language with localized assets (a polish install has only localized_polish IWDs). The value of the config
// waits for a vid_restart (loc_language is latched), so the language of a player is the one of his localized IWDs.
// CoD2x: loc_language 0 ("auto", the default and the value of the vanilla configs) is that automatic language - counted
// only from the localized assets of the install, not from the CoD2x IWD (it makes english and the CoD2x languages
// available everywhere, a polish install would start in english). Any other value is a language chosen by the player,
// applied after the config is executed. English texts on a localized install: loc_forceEnglish 1.
// While the game runs, loc_language is the real language (the engine reads it), the config gets "auto" back.

static bool language_auto = true;   // loc_language is "auto"
static int language_resolved = -1;  // the language set for "auto"
static int language_install = 0;    // the language "auto" resolves to (the menu needs it also when not auto)

// True if the install has localized assets of the language (localized IWDs or folders), the CoD2x IWD not counted.
// The fallback marks must be restored before.
static bool Language_HasOwnAssets(int language)
{
    for (searchpath_t* sp = fs_searchpaths; sp; sp = sp->next) {
        if (sp->localized && sp->language == language && !Language_IsCoD2xIwd(sp))
            return true;
    }
    return false;
}

// The automatic language, like vanilla: english, else the first language with localized assets in the install
static int Language_GetInstallLanguage()
{
    for (int i = 0; i < LANGUAGE_COUNT; i++) {
        if (Language_HasOwnAssets(i))
            return i;
    }
    return 0; // nothing localized in the install: english of the CoD2x IWD
}

// True if the dvar is set on the command line ("+set loc_language czech")
static bool Language_IsSetOnCommandLine(const char* name)
{
    const char* cmd = GetCommandLineA();
    int len = (int)strlen(name);
    for (const char* p = cmd; p && *p; p++) {
        if (I_strnicmp(p, name, len) == 0)
            return true;
    }
    return false;
}

// Sets the language now (loc_language is latched - a latched value would wait for a vid_restart)
static void Language_Set(int language)
{
    loc_language->value.integer = language;
    loc_language->latchedValue.integer = language;
    loc_language->modified = true;
}

// Before the language is used (SEH_UpdateLanguageInfo): "auto" becomes the language of the install
static void Language_ResolveAuto()
{
    if (!loc_language)
        return;
    Language_RestoreFallback();

    language_install = Language_GetInstallLanguage();

    int value = loc_language->value.integer;
    if (value == 0 || (language_auto && value == language_resolved)) {
        bool changed = !language_auto || language_resolved < 0;
        language_auto = true;
        language_resolved = language_install;
        Language_Set(language_resolved);
        if (changed)
            Com_Printf("Language: %s (auto - localized IWDs of the game)\n", Language_GetCurrentName());
    } else if (language_auto) {
        language_auto = false;
        Com_Printf("Language: %s\n", Language_GetCurrentName());
    }
}

// Com_Init, after the config: the language and loc_forceEnglish of the config (latched until now), unless set on the
// command line
static int Language_ApplyConfig()
{
    if (loc_language) {
        if (Language_IsSetOnCommandLine("loc_language"))
            loc_language->latchedValue = loc_language->value;
        else if (loc_language->latchedValue.integer != loc_language->value.integer)
            Language_Set(loc_language->latchedValue.integer);
    }
    if (loc_forceEnglish) {
        if (Language_IsSetOnCommandLine("loc_forceEnglish"))
            loc_forceEnglish->latchedValue = loc_forceEnglish->value;
        else
            loc_forceEnglish->value = loc_forceEnglish->latchedValue;
    }
    return SEH_UpdateLanguageInfo();
}

// Writes the archived dvars into the config - "auto" is written as "auto", not as the language it is now
// 0043a3d0    void Dvar_WriteVariables(int32_t f)
static void Dvar_WriteVariables(int f)
{
    // CoD2x: loc_language "auto"
    // The latched value is written (a value waiting for vid_restart is saved)
    int latched = loc_language ? loc_language->latchedValue.integer : 0;
    if (loc_language && language_auto && latched == loc_language->value.integer)
        loc_language->latchedValue.integer = 0;
    // CoD2x: End

    // Call the original function
    ASM_CALL(RETURN_VOID, 0x0043a3d0, 1, PUSH(f));

    // CoD2x: loc_language "auto"
    if (loc_language)
        loc_language->latchedValue.integer = latched;
    // CoD2x: End
}


// Options menu (ui/options_game.menu of iw_CoD2x_02, the vanilla one shows the language only with developer 1):
//   - onOpen "uiScript getLanguage": ui_language = loc_language
//   - the list ui_language: "uiScript verifyLanguage" skips the languages without assets, ui_languagechanged shows
//     the button "Apply language change"
//   - the restart popup (ui/language_restart.menu) "uiScript updateLanguage": loc_language = ui_language, vid_restart
// CoD2x: 0 in the list is "As installed" (loc_language auto - it holds the resolved language while the game runs), and
// ui_forceEnglish (the YES/NO "Force English Texts") and ui_fontSet (the font style, the value of cl_fontSet) are applied with the
// language. loc_forceEnglish is latched and vid_restart does not apply it (only loc_language is registered again there),
// so it is set directly. ui_fontSetAvailable is 0 when the selected language has no fonts in the CoD4 set (the font
// style is grey then, the fonts of the game are used). With English texts the language is grey "As installed" and
// applied as auto - it would change only the fonts (and the voices of an install with more localized languages).

static bool Font_FileExists(const char* path);
static void Font_SetApplied();

// ui_fontSet: the value of cl_fontSet
static int Language_GetMenuFontSet()
{
    return cl_fontSet ? cl_fontSet->value.integer : FONT_SET_ORIGINAL;
}

// True if the CoD4 font set has the fonts of the language (the western languages share fonts/cod4/<font>, a language
// with own fonts needs fonts/cod4/<language>/<font>)
static bool Language_IsMenuFontSetAvailable(int language)
{
    const char* set = font_setNames[FONT_SET_COD4];
    const char* folder = Language_GetFontFolder(language);
    char path[256];
    if (folder)
        snprintf(path, sizeof(path), "fonts/%s/%s/normalFont", set, folder);
    else
        snprintf(path, sizeof(path), "fonts/%s/normalFont", set);
    return Font_FileExists(path);
}

// Value of loc_language for the menu (getLanguage: ui_language = loc_language): 0 for "auto", not the resolved language.
// Sets ui_forceEnglish and ui_fontSet too.
// Hooked Dvar_GetInt("loc_language") call of UI_RunMenuScript (getLanguage)
static int Language_GetMenuLanguage(const char* name)
{
    if (loc_forceEnglish)
        Dvar_SetIntByName("ui_forceEnglish", loc_forceEnglish->value.boolean ? 1 : 0);
    Dvar_SetIntByName("ui_fontSet", Language_GetMenuFontSet());
    if (language_auto)
        return 0;
    return Dvar_GetInt(name);
}

// Shows the button "Apply language change" (ui_languagechanged) if the menu differs from the current settings. Vanilla
// compares ui_language with loc_language only: "Automatic" while auto would differ from the resolved language, the
// resolved language would not. For "auto", "Automatic" and the resolved language are the same.
// 00534780    void UI_VerifyLanguage()
static void UI_VerifyLanguage()
{
    // Call the original function
    ASM_CALL(RETURN_VOID, 0x00534780, 0);

    // CoD2x: "auto", ui_forceEnglish and ui_fontSet
    if (!loc_language || !loc_forceEnglish)
        return;
    int selected = Dvar_GetInt("ui_language"); // after the original, which skipped the languages without assets
    if (Dvar_GetInt("ui_forceEnglish"))
        selected = 0; // English texts: the language is grey "As installed" (ui_language is kept for "No")
    bool languageChanged;
    if (language_auto)
        languageChanged = selected != 0 && selected != language_resolved;
    else
        languageChanged = selected != loc_language->value.integer;
    bool forceEnglishChanged = (Dvar_GetInt("ui_forceEnglish") != 0) != (loc_forceEnglish->value.boolean != 0);
    bool fontSetChanged = Dvar_GetInt("ui_fontSet") != Language_GetMenuFontSet();
    Dvar_SetIntByName("ui_languagechanged", (languageChanged || forceEnglishChanged || fontSetChanged) ? 1 : 0);

    // The font style of the selected language
    Dvar_SetIntByName("ui_fontSetAvailable", Language_IsMenuFontSetAvailable(selected == 0 ? language_install : selected) ? 1 : 0);
    // CoD2x: End
}

// updateLanguage: loc_language = ui_language (latched, applied by the vid_restart after it), loc_forceEnglish =
// ui_forceEnglish (set directly, vid_restart does not apply its latched value)
// Hooked Dvar_SetIntByName("loc_language", ui_language) call of UI_RunMenuScript (updateLanguage)
static dvar_t* Language_SetFromMenu(const char* name, int value)
{
    if (loc_forceEnglish) {
        bool forceEnglish = Dvar_GetInt("ui_forceEnglish") != 0;
        loc_forceEnglish->value.boolean = forceEnglish;
        loc_forceEnglish->latchedValue.boolean = forceEnglish;
        loc_forceEnglish->modified = true;
        // English texts: the language of the install (the language would change only the fonts and the voices)
        if (forceEnglish)
            value = 0;
    }
    // The font set is registered by the vid_restart after this (language_frame must not add another vid_restart)
    int fontSet = Dvar_GetInt("ui_fontSet");
    if (cl_fontSet && fontSet != Language_GetMenuFontSet()) {
        Dvar_SetInt(cl_fontSet, fontSet);
        Font_SetApplied();
    }
    // "As installed" or the resolved language while auto: stays auto (Language_ResolveAuto)
    return Dvar_SetIntByName(name, value);
}


// ======================================================================================================
// Fonts
// ======================================================================================================

// True if the file is in the search path
static bool Font_FileExists(const char* path)
{
    return FS_ReadFile(path, NULL) > 0;
}

// Path of a font of the font set cl_fontSet into path, if the set has the font for the current language.
static bool Font_GetSetPath(const char* fontName, char* path, size_t size)
{
    if (!cl_fontSet || cl_fontSet->value.integer == FONT_SET_ORIGINAL)
        return false;

    const char* set = font_setNames[cl_fontSet->value.integer];
    const char* folder = Language_GetFontFolder(loc_language ? loc_language->value.integer : 0);
    if (folder)
        snprintf(path, size, "fonts/%s/%s/%s", set, folder, fontName);
    else
        snprintf(path, size, "fonts/%s/%s", set, fontName);

    if (Font_FileExists(path))
        return true;

    Com_Printf("^3Font set '%s' has no font %s, the font of the game is used\n", set, path);
    return false;
}

// Remembers the font set the fonts were registered with
static void Font_SetApplied()
{
    language_fontSetApplied = cl_fontSet ? cl_fontSet->value.integer : FONT_SET_ORIGINAL;
    language_fontsRegistered = true;
}

// Registers the fonts of the main menu (assetGlobalDef font, smallFont, bigFont, extraBigFont, boldFont, consoleFont).
// fontName is the font name ("normalFont", "smallFont", ...), defaultPath the path from the menu.
// 00545740    Font_s* RegisterFontHack(char* fontName @ ecx, char* defaultPath, int32_t imageTrack)
static fontHandle_t* RegisterFontHack(const char* fontName, const char* defaultPath, int imageTrack)
{
    char path[256];

    // CoD2x: font set
    Font_SetApplied();
    if (Font_GetSetPath(fontName, path, sizeof(path)))
        return gfx_funcs_R_RegisterFont(path, imageTrack);
    // CoD2x: End

    switch (loc_language->value.integer)
    {
        case 6:  snprintf(path, sizeof(path), "fonts/russian/%s", fontName); break;
        case 7:  snprintf(path, sizeof(path), "fonts/polish/%s", fontName); break;
        case 8:  snprintf(path, sizeof(path), "fonts/korean/%s", fontName); break;
        case 9:  snprintf(path, sizeof(path), "fonts/taiwanese/%s", fontName); break;
        case 10: snprintf(path, sizeof(path), "fonts/japanese/%s", fontName); break;
        case 11: snprintf(path, sizeof(path), "fonts/chinese/%s", fontName); break;
        case 12: snprintf(path, sizeof(path), "fonts/thai/%s", fontName); break;
        case 14: snprintf(path, sizeof(path), "fonts/czech/%s", fontName); break;
        default: snprintf(path, sizeof(path), "%s", defaultPath); break; // Com_sprintf(path, 256, defaultPath)
    }

    return gfx_funcs_R_RegisterFont(path, imageTrack);
}

static fontHandle_t* RegisterFontHack_Win32(const char* defaultPath, int imageTrack)
{
    const char* fontName;
    ASM( movr, fontName, "ecx" );

    return RegisterFontHack(fontName, defaultPath, imageTrack);
}

// Console font in CL_InitRenderer: "fonts/<language>/consoleFont" or "fonts/consoleFont".
// The call of the renderer slot is replaced by a call of this function.
// 1000bd00 (gfx)    Font_s* R_RegisterFont(char* name, int32_t imageTrack)
static fontHandle_t* R_RegisterFont(const char* name, int imageTrack)
{
    // CoD2x: font set, and the czech console font (CL_InitRenderer does not have czech, the menus have)
    char path[256];
    if (Font_GetSetPath("consoleFont", path, sizeof(path)))
        return gfx_funcs_R_RegisterFont(path, imageTrack);

    if (loc_language && loc_language->value.integer == 14 && Font_FileExists("fonts/czech/consoleFont"))
        return gfx_funcs_R_RegisterFont("fonts/czech/consoleFont", imageTrack);
    // CoD2x: End

    return gfx_funcs_R_RegisterFont(name, imageTrack);
}



// ======================================================================================================
// Module
// ======================================================================================================

/** Called every frame on frame start. */
void language_frame() {
    // Font set changed: register the fonts again
    if (cl_fontSet && language_fontsRegistered && cl_fontSet->value.integer != language_fontSetApplied) {
        Font_SetApplied(); // once, also if the restart fails
        Com_Printf("Font set changed to '%s', restarting video\n", font_setNames[cl_fontSet->value.integer]);
        Cbuf_AddText("vid_restart\n");
    }

    // Language of the texts for the server (loc_language and loc_forceEnglish are latched, they change with a restart)
    if (cl_language && loc_language && strcmp(cl_language->value.string, Language_GetTextName()) != 0)
        Dvar_SetString(cl_language, Language_GetTextName());
}

/** Called only once on game start after common inicialization. Used to initialize variables, cvars, etc. */
void language_init() {
    cl_fontSet = Dvar_RegisterEnum("cl_fontSet", font_setNames, FONT_SET_ORIGINAL, (enum dvarFlags_e)(DVAR_ARCHIVE));

    // Sent to the server in the userinfo, read by GSC player getLanguage()
    cl_language = Dvar_RegisterString("cl_language", Language_GetTextName(),(enum dvarFlags_e)(DVAR_USERINFO | DVAR_ROM));
}

/** Called before the entry point is called. Used to patch the memory. */
void language_patch() {
    // Fonts of the main menu
    patch_call(0x005458ef, (unsigned int)RegisterFontHack_Win32); // Asset_Parse - font
    patch_call(0x0054596b, (unsigned int)RegisterFontHack_Win32); // Asset_Parse - smallFont
    patch_call(0x005459d6, (unsigned int)RegisterFontHack_Win32); // Asset_Parse - bigFont
    patch_call(0x00545a41, (unsigned int)RegisterFontHack_Win32); // Asset_Parse - extraBigFont
    patch_call(0x00545aac, (unsigned int)RegisterFontHack_Win32); // Asset_Parse - boldFont
    patch_call(0x00545b17, (unsigned int)RegisterFontHack_Win32); // Asset_Parse - consoleFont

    // Console font: "call dword [gfx_funcs.R_RegisterFont]" (6 bytes) -> "call R_RegisterFont; nop"
    patch_call(0x0040faea, (unsigned int)R_RegisterFont); // CL_InitRenderer
    patch_nop(0x0040faef, 1);

    // Translations: only the language of the player from .str files
    patch_call(0x004ad1c4, (unsigned int)SE_LoadFileData); // SE_Load

    // loc_language as an enum with the names of the languages
    patch_call(0x004257e9, (unsigned int)SEH_InitLanguage); // FS_InitFilesystem

    // The CoD2x localized IWDs (localized_CoD2x_*) are English localized assets
    patch_call(0x004249df, (unsigned int)IwdFileLanguage_Win32); // FS_AddIwdFilesForGameDirectory
    patch_call(0x0042477c, (unsigned int)IwdFileLanguage_Win32); // iwdsort
    patch_call(0x00424785, (unsigned int)IwdFileLanguage_Win32); // iwdsort

    // English localized assets as the fallback of the current language, locked localized IWDs
    patch_call(0x004257f3, (unsigned int)FS_Startup); // FS_InitFilesystem
    patch_call(0x00425958, (unsigned int)FS_Startup); // FS_Restart
    patch_call(0x0043bde3, (unsigned int)FS_Startup); // restricted demo mode (fs_restrict)
    patch_call(0x0040d962, (unsigned int)SEH_UpdateLanguageInfo); // vid_restart
    patch_call(0x00425827, (unsigned int)SEH_UpdateLanguageInfo); // FS_InitFilesystem
    patch_call(0x0042598f, (unsigned int)SEH_UpdateLanguageInfo); // FS_Restart
    patch_call(0x00432271, (unsigned int)SEH_UpdateLanguageInfo); // sub_432210

    // loc_language "auto" (0): the language of the localized IWDs of the install; a chosen one from the config
    patch_call(0x004345f0, (unsigned int)Language_ApplyConfig);    // Com_Init, after the config
    patch_call(0x00434acd, (unsigned int)Dvar_WriteVariables);     // Com_WriteConfigToFile

    // Options menu: "As installed" (loc_language auto) and "Force English Texts" (loc_forceEnglish)
    patch_call(0x00535f26, (unsigned int)Language_GetMenuLanguage); // UI_RunMenuScript - getLanguage
    patch_call(0x00535f3c, (unsigned int)UI_VerifyLanguage);        // UI_RunMenuScript - getLanguage, verifyLanguage
    patch_call(0x00535f89, (unsigned int)Language_SetFromMenu);     // UI_RunMenuScript - updateLanguage
    patch_call(0x00535f91, (unsigned int)UI_VerifyLanguage);        // UI_RunMenuScript - updateLanguage
}
