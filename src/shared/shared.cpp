#include "shared.h"

#include <cstdio>
#include <cctype>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <time.h>
#endif

/*
 * Compares two version strings.
 * Versions are expected in the format: "1.4.4.2" or "1.4.4.2-test.1".
 *
 * Returns:
 *   <0 if ver1 is lower than ver2,
 *    0 if they are equal,
 *   >0 if ver1 is greater than ver2.
 *
 * A version without a pre-release part is considered higher (more stable)
 * than one with a pre-release part if the main parts are identical.
 */
int version_compare(const char *v1, const char *v2, bool* firstIsPrerelease, bool* secondIsPrerelease) {
    if (firstIsPrerelease) *firstIsPrerelease = false;
    if (secondIsPrerelease) *secondIsPrerelease = false;

    if (v1 == v2) return 0;
    if (!v1) return -1;
    if (!v2) return 1;

    const char *p1 = v1, *p2 = v2;

    // Step 1: compare numeric core (e.g. 1.2.3.4)
    while (1) {
        int end1 = (*p1 == '-' || *p1 == '\0');
        int end2 = (*p2 == '-' || *p2 == '\0');

        if (end1 && end2) break;

        uint64_t n1 = 0, n2 = 0;

        if (!end1) {
            char *q;
            n1 = strtoull(p1, &q, 10);
            if (q == p1)  break;  // invalid token
            p1 = (*q == '.') ? q + 1 : q;
        }

        if (!end2) {
            char *q;
            n2 = strtoull(p2, &q, 10);
            if (q == p2)  break;  // invalid token
            p2 = (*q == '.') ? q + 1 : q;
        }

        if (n1 < n2) return -1;
        if (n1 > n2) return 1;
    }

    // Step 2: handle pre-release presence
    // "1.2.3.4" > "1.2.3.4-alpha"
    const char *pre1 = (*p1 == '-') ? p1 + 1 : NULL;
    const char *pre2 = (*p2 == '-') ? p2 + 1 : NULL;

    if (firstIsPrerelease) *firstIsPrerelease = (pre1 != NULL);
    if (secondIsPrerelease) *secondIsPrerelease = (pre2 != NULL);

    if (!pre1 && !pre2) return 0;
    if (!pre1) return 1;
    if (!pre2) return -1;

    // Step 3: compare pre-release tokens (e.g. "alpha", "test.2", etc.)
    const char *t1 = pre1, *t2 = pre2;

    while (1) {
        const char *e1 = t1; while (*e1 && *e1 != '.') ++e1;
        const char *e2 = t2; while (*e2 && *e2 != '.') ++e2;
        size_t len1 = (size_t)(e1 - t1);
        size_t len2 = (size_t)(e2 - t2);

        int num1 = len1 && strspn(t1, "0123456789") == len1;
        int num2 = len2 && strspn(t2, "0123456789") == len2;
        int result = 0;

        if (num1 && num2) {
            // Compare numerically: "test.2" < "test.10"
            char buf1[32] = {0}, buf2[32] = {0};
            if (len1 < sizeof(buf1) && len2 < sizeof(buf2)) {
                memcpy(buf1, t1, len1);
                memcpy(buf2, t2, len2);
                uint64_t v1n = strtoull(buf1, NULL, 10);
                uint64_t v2n = strtoull(buf2, NULL, 10);
                result = (v1n < v2n) ? -1 : (v1n > v2n);
            } else {
                result = (len1 < len2) ? -1 : (len1 > len2);
            }
        } else if (num1 != num2) {
            // Numeric tokens are always lower than alphanumeric
            result = num1 ? -1 : 1;
        } else {
            // Compare alphanumerics case-insensitively: "alpha" vs "beta"
            size_t n = (len1 < len2) ? len1 : len2;
            for (size_t i = 0; i < n && !result; ++i) {
                unsigned char c1 = (unsigned char)tolower((unsigned char)t1[i]);
                unsigned char c2 = (unsigned char)tolower((unsigned char)t2[i]);
                if (c1 != c2) result = (c1 < c2) ? -1 : 1;
            }
            if (!result && len1 != len2) {
                result = (len1 < len2) ? -1 : 1;
            }
        }

        if (result) return result;

        // Advance to next token
        t1 = (*e1 == '.') ? e1 + 1 : e1;
        t2 = (*e2 == '.') ? e2 + 1 : e2;

        if (!*t1 && !*t2) return 0;
        if (!*t1) return -1;
        if (!*t2) return 1;
    }
}


void escape_string(char* buffer, size_t bufferSize, const void* data, size_t length) {
    // Loop through the data char by char and escape invisible characters into buffer
    size_t buffer_index = 0;
    for (size_t i = 0; i < length && buffer_index < (size_t)(bufferSize - 5); i++) { // Reserve 5 bytes for worst-case scenario
        char c = ((char*)data)[i]; // Process data directly without skipping
        switch (c) {
            case '\n':
                buffer[buffer_index++] = '\\';
                buffer[buffer_index++] = 'n';
                break;
            case '\r':
                buffer[buffer_index++] = '\\';
                buffer[buffer_index++] = 'r';
                break;
            case '\t':
                buffer[buffer_index++] = '\\';
                buffer[buffer_index++] = 't';
                break;
            default:
                if (c < 32 || c > 126) { // Non-printable characters
                    if (buffer_index + 4 < bufferSize) { // Ensure enough space for "\\xNN"
                        snprintf(&buffer[buffer_index], 5, "\\x%02X", (unsigned char)c);
                        buffer_index += 4;
                    } else {
                        break; // Exit loop if there isn't enough space
                    }
                } else {
                    buffer[buffer_index++] = c;
                }
                break;
        }
    }
    buffer[buffer_index] = '\0'; // Ensure null termination
}

// CRC16-CCITT (poly 0x1021, initial value 0xFFFF)
uint16_t crc16_ccitt(const uint8_t* data, size_t length)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; ++j) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc <<= 1;
        }
    }
    return crc;
}


int base64_encode(const uint8_t* input, size_t len, char* output, size_t out_size)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (!input || !output) return -1;

    size_t i = 0, j = 0;
    while (i < len) {
        size_t remain = len - i;
        uint32_t octet_a = i < len ? input[i++] : 0;
        uint32_t octet_b = i < len ? input[i++] : 0;
        uint32_t octet_c = i < len ? input[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        if (j + 4 >= out_size) return -2; // Not enough space

        output[j++] = alphabet[(triple >> 18) & 0x3F];
        output[j++] = alphabet[(triple >> 12) & 0x3F];
        output[j++] = (remain > 1) ? alphabet[(triple >> 6) & 0x3F] : '=';
        output[j++] = (remain > 2) ? alphabet[triple & 0x3F] : '=';
    }
    if (j < out_size)
        output[j] = '\0';
    else
        return -2; // Not enough space for null terminator

    return (int)j; // Return encoded length
}

int base64_decode(const char* input, uint8_t* output, size_t out_size)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    if (!input || !output) return -1;

    // Build reverse lookup table
    int8_t reverse_table[256];
    memset(reverse_table, -1, sizeof(reverse_table));
    for (int i = 0; i < 64; ++i) {
        reverse_table[(unsigned char)alphabet[i]] = i;
    }

    size_t in_len = strlen(input);
    size_t i = 0, j = 0;
    while (i < in_len) {
        int vals[4] = {0, 0, 0, 0};
        int pad = 0;
        for (int k = 0; k < 4 && i < in_len; ++k, ++i) {
            char c = input[i];
            if (c == '=') {
                vals[k] = 0;
                pad++;
            } else {
                int8_t v = reverse_table[(unsigned char)c];
                if (v == -1) return -2; // Invalid character
                vals[k] = v;
            }
        }
        if (j + 3 > out_size) return -3; // Not enough space

        output[j++] = (vals[0] << 2) | (vals[1] >> 4);
        if (pad < 2) output[j++] = ((vals[1] & 0xF) << 4) | (vals[2] >> 2);
        if (pad < 1) output[j++] = ((vals[2] & 0x3) << 6) | vals[3];
    }
    return (int)j;
}



/**
 * Get current UTC wall-clock time in milliseconds since Unix epoch (1970-01-01).
 *
 * - On Windows uses GetSystemTimeAsFileTime (fast, ~15 ms precision).
 * - On Linux uses CLOCK_REALTIME_COARSE (fast, ~1–4 ms precision).
 * - ⚠ Value may jump forward/backward if the system clock changes (NTP, manual adjustment).
 *
 * Example:
 *   uint64_t now = time_utc_ms();
 *   printf("Current UTC time: %llu ms\n", (unsigned long long) now);
 */
uint64_t time_utc_ms(void) {
#if defined(_WIN32)
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER t;
    t.LowPart  = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return t.QuadPart / 10000ULL - 11644473600000ULL;
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME_COARSE, &ts);   // faster, lower precision
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
#endif
}

/**
 * Get a monotonic tick counter in milliseconds.
 *
 * - On Windows uses QueryPerformanceCounter.
 * - On Linux uses CLOCK_MONOTONIC.
 * - Monotonic: the value never jumps due to system clock changes.
 * - Ideal for measuring durations, intervals, and timeouts.
 * - Resolution: usually microseconds (rounded to ms).
 *
 * Example:
 *   uint64_t start = ticks_ms();
 *   do_work();
 *   uint64_t elapsed = ticks_ms() - start;
 *   printf("Elapsed: %llu ms\n", (unsigned long long) elapsed);
 */
uint64_t ticks_ms(void) {
#if defined(_WIN32)
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (uint64_t)(counter.QuadPart * 1000ULL / freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL;
#endif
}


/**
 * Convert a UTC timestamp (milliseconds since Unix epoch) into ISO8601 string.
 * 
 * Output format: "YYYY-MM-DDTHH:MM:SS.mmmZ"
 * - Always UTC, with millisecond precision.
 * - Example: 2025-08-24T21:34:15.123Z
 *
 * Params:
 *   ms_epoch   - timestamp in ms since 1970-01-01
 *   buf        - destination buffer
 *   buf_size   - size of buffer (at least 25 recommended)
 *
 * Returns: buf pointer (for convenience).
 */
char* time_to_iso8601(uint64_t ms_epoch, char* buf, size_t buf_size) {
    time_t seconds = (time_t)(ms_epoch / 1000ULL);
    int millis     = (int)(ms_epoch % 1000ULL);

    struct tm tm_utc;
#if defined(_WIN32)
    gmtime_s(&tm_utc, &seconds);   // Windows safe version
#else
    gmtime_r(&seconds, &tm_utc);   // POSIX safe version
#endif

    // Format: "YYYY-MM-DDTHH:MM:SS.mmmZ"
    snprintf(buf, buf_size,
             "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
             tm_utc.tm_year + 1900,
             tm_utc.tm_mon + 1,
             tm_utc.tm_mday,
             tm_utc.tm_hour,
             tm_utc.tm_min,
             tm_utc.tm_sec,
             millis);

    return buf;
}