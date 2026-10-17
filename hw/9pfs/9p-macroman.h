/*
 * 9p-macroman.h - Mac Roman <-> UTF-8 filename encoding conversion
 *
 * The classic Mac OS (System 7, Mac OS 8/9) uses Mac Roman encoding for
 * filenames. The classicvirtio 9P driver sends raw Mac Roman bytes.
 * Windows NTFS stores filenames as UTF-16 internally, and the MinGW CRT
 * presents them as UTF-8 via the narrow-char APIs.
 *
 * Without conversion, filenames with characters 0x80-0xFF get mangled
 * on the round-trip: guest creates "MacBench® 5.0" (Mac Roman 0xA8 = ®),
 * Windows stores the raw byte 0xA8, which is NOT valid UTF-8. On readback,
 * the name comes back garbled and resource forks / FinderInfo can no longer
 * be matched to their files.
 *
 * Additionally, Mac OS allows characters in filenames that Windows forbids:
 * control characters (notably 0x0D carriage return, used in the classic
 * "Icon\r" file for custom folder icons), and the characters " * < > ? |
 * which are legal on Mac but not Windows.  These are escaped using Unicode
 * Private Use Area codepoints (U+F001-F027), compatible with Apple's SFM
 * (Services for Macintosh) mapping — the same mapping macOS's own SMB
 * client uses.
 *
 * This header provides bidirectional conversion:
 *   macroman_to_utf8() - for guest->host paths (create, rename, open)
 *   utf8_to_macroman() - for host->guest paths (readdir)
 *
 * Copyright (c) 2025
 * License: GNU GPL v2 or later
 */

#ifndef QEMU_9P_MACROMAN_H
#define QEMU_9P_MACROMAN_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "qemu/osdep.h"

/*
 * Mac Roman to Unicode mapping table for bytes 0x80-0xFF.
 * Bytes 0x00-0x7F are identical to ASCII/UTF-8.
 */
static const uint16_t macroman_to_unicode[128] = {
    /* 0x80 */ 0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1,
    /* 0x88 */ 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5, 0x00E7, 0x00E9, 0x00E8,
    /* 0x90 */ 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3,
    /* 0x98 */ 0x00F2, 0x00F4, 0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC,
    /* 0xA0 */ 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6, 0x00DF,
    /* 0xA8 */ 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8,
    /* 0xB0 */ 0x221E, 0x00B1, 0x2264, 0x2265, 0x00A5, 0x00B5, 0x2202, 0x2211,
    /* 0xB8 */ 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8,
    /* 0xC0 */ 0x00BF, 0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB,
    /* 0xC8 */ 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5, 0x0152, 0x0153,
    /* 0xD0 */ 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA,
    /* 0xD8 */ 0x00FF, 0x0178, 0x2044, 0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02,
    /* 0xE0 */ 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    /* 0xE8 */ 0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4,
    /* 0xF0 */ 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9, 0x0131, 0x02C6, 0x02DC,
    /* 0xF8 */ 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

/*
 * =========================================================================
 *  Windows Filename Character Escaping (SFM-compatible)
 *
 *  Windows NTFS forbids control characters (0x00-0x1F) and " * : < > ? | \
 *  in filenames.  Mac OS allows all of these except ':' (its path separator).
 *  The most important case is 0x0D (carriage return) in the classic Mac
 *  "Icon\r" file used for custom folder icons.
 *
 *  We escape these using Unicode Private Use Area codepoints, following
 *  Apple's SFM (Services for Macintosh) mapping.  This is the same mapping
 *  macOS's own SMB client uses, so escaped filenames will look correct if
 *  the share is ever accessed from a Mac host too.
 *
 *  Mapping:
 *    0x01-0x1F  ->  U+F001-U+F01F  (control characters)
 *    0x22 "     ->  U+F020
 *    0x2A *     ->  U+F021
 *    0x3C <     ->  U+F023
 *    0x3E >     ->  U+F024
 *    0x3F ?     ->  U+F025
 *    0x7C |     ->  U+F027
 *
 *  Note: 0x2F '/' and 0x5C '\' are path separators and should never appear
 *  in a single 9P name component.  0x3A ':' is the Mac path separator and
 *  also never appears in Mac filenames (and is handled separately by 9P).
 * =========================================================================
 */

/*
 * Map an ASCII byte to its SFM Private Use Area escape codepoint.
 * Returns the PUA codepoint if the character needs escaping, or 0 if not.
 */
static inline uint16_t win32_escape_char(uint8_t ch)
{
    if (ch >= 0x01 && ch <= 0x1F) {
        return 0xF000 + ch;         /* U+F001 - U+F01F */
    }
    switch (ch) {
    case '"':  return 0xF020;
    case '*':  return 0xF021;
    case '<':  return 0xF023;
    case '>':  return 0xF024;
    case '?':  return 0xF025;
    case '|':  return 0xF027;
    default:   return 0;
    }
}

/*
 * Check if a Unicode codepoint is an SFM PUA escape.
 * Returns the original ASCII byte (1-0x7C), or 0 if not an SFM escape.
 */
static inline uint8_t win32_unescape_char(uint16_t cp)
{
    if (cp >= 0xF001 && cp <= 0xF01F) {
        return (uint8_t)(cp - 0xF000);  /* control chars */
    }
    switch (cp) {
    case 0xF020: return '"';
    case 0xF021: return '*';
    case 0xF023: return '<';
    case 0xF024: return '>';
    case 0xF025: return '?';
    case 0xF027: return '|';
    default:     return 0;
    }
}

/*
 * Encode a single Unicode codepoint as UTF-8.
 * Returns the number of bytes written (1-3 for BMP characters).
 * Buffer must have room for at least 3 bytes.
 */
static inline int unicode_to_utf8(uint16_t cp, uint8_t *out)
{
    if (cp < 0x80) {
        out[0] = (uint8_t)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = 0xC0 | (cp >> 6);
        out[1] = 0x80 | (cp & 0x3F);
        return 2;
    } else {
        out[0] = 0xE0 | (cp >> 12);
        out[1] = 0x80 | ((cp >> 6) & 0x3F);
        out[2] = 0x80 | (cp & 0x3F);
        return 3;
    }
}

/*
 * Convert a Mac Roman encoded string to UTF-8 for use on Windows.
 * Returns a newly allocated string (caller must g_free).
 *
 * Mac Roman bytes 0x00-0x7F pass through unchanged (they are ASCII),
 * EXCEPT characters that are illegal in Windows filenames — these are
 * escaped to SFM Private Use Area codepoints (3 UTF-8 bytes each).
 *
 * Bytes 0x80-0xFF are mapped via the table to Unicode, then encoded as UTF-8.
 * Worst case expansion is 3x (every byte maps to a 3-byte UTF-8 sequence).
 */
static inline char *macroman_to_utf8(const char *macroman)
{
    const uint8_t *src = (const uint8_t *)macroman;
    size_t srclen = strlen(macroman);
    /* Worst case: every byte becomes 3 UTF-8 bytes */
    uint8_t *buf = g_malloc(srclen * 3 + 1);
    uint8_t *dst = buf;

    for (size_t i = 0; i < srclen; i++) {
        uint8_t ch = src[i];
        if (ch < 0x80) {
            /* ASCII range — check for Windows-illegal characters */
            uint16_t esc = win32_escape_char(ch);
            if (esc) {
                /* Escape to SFM PUA codepoint (always 3 UTF-8 bytes) */
                dst += unicode_to_utf8(esc, dst);
            } else {
                *dst++ = ch;
            }
        } else {
            /* Mac Roman high byte — convert to Unicode */
            uint16_t cp = macroman_to_unicode[ch - 0x80];
            dst += unicode_to_utf8(cp, dst);
        }
    }
    *dst = '\0';
    return (char *)buf;
}

/*
 * Decode one UTF-8 character from src, return the Unicode codepoint.
 * Advances *pos by the number of bytes consumed.
 * Returns 0xFFFD (replacement character) on invalid sequences.
 */
static inline uint16_t utf8_decode_one(const uint8_t *src, size_t len,
                                       size_t *pos)
{
    size_t i = *pos;
    uint8_t ch = src[i];

    if (ch < 0x80) {
        *pos = i + 1;
        return ch;
    } else if ((ch & 0xE0) == 0xC0 && i + 1 < len) {
        uint16_t cp = ((ch & 0x1F) << 6) | (src[i + 1] & 0x3F);
        *pos = i + 2;
        return cp;
    } else if ((ch & 0xF0) == 0xE0 && i + 2 < len) {
        uint16_t cp = ((ch & 0x0F) << 12) |
                      ((src[i + 1] & 0x3F) << 6) |
                      (src[i + 2] & 0x3F);
        *pos = i + 3;
        return cp;
    } else {
        /* Invalid or 4-byte sequence (beyond BMP) — skip one byte */
        *pos = i + 1;
        return 0xFFFD;
    }
}

/*
 * Convert a UTF-8 string (from Windows filesystem) to Mac Roman encoding.
 * Returns a newly allocated string (caller must g_free).
 *
 * SFM Private Use Area codepoints (U+F001-F027) are unescaped back to
 * the original ASCII bytes that were escaped during macroman_to_utf8().
 *
 * Unicode codepoints that have a Mac Roman mapping are converted.
 * Codepoints without a mapping are replaced with '?' (0x3F).
 * ASCII (0x00-0x7F) passes through unchanged.
 */
static inline char *utf8_to_macroman(const char *utf8)
{
    const uint8_t *src = (const uint8_t *)utf8;
    size_t srclen = strlen(utf8);
    /* Output can never be longer than input (UTF-8 is always >= Mac Roman) */
    uint8_t *buf = g_malloc(srclen + 1);
    uint8_t *dst = buf;
    size_t pos = 0;

    while (pos < srclen) {
        uint16_t cp = utf8_decode_one(src, srclen, &pos);

        if (cp < 0x80) {
            /* ASCII — pass through */
            *dst++ = (uint8_t)cp;
        } else {
            /* Check if this is an SFM PUA escape first */
            uint8_t unesc = win32_unescape_char(cp);
            if (unesc) {
                *dst++ = unesc;
                continue;
            }
            /* Search the Mac Roman table for this codepoint */
            uint8_t found = 0;
            for (int j = 0; j < 128; j++) {
                if (macroman_to_unicode[j] == cp) {
                    *dst++ = (uint8_t)(j + 0x80);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                *dst++ = '?';  /* No Mac Roman equivalent */
            }
        }
    }
    *dst = '\0';
    return (char *)buf;
}

/*
 * Check if a string contains any bytes that need encoding conversion.
 * This includes non-ASCII bytes (0x80-0xFF) for Mac Roman conversion,
 * and Windows-illegal characters that need SFM escaping.
 */
static inline bool needs_encoding_conversion(const char *s)
{
    for (const uint8_t *p = (const uint8_t *)s; *p; p++) {
        if (*p >= 0x80) {
            return true;
        }
        if (win32_escape_char(*p)) {
            return true;
        }
    }
    return false;
}

#endif /* QEMU_9P_MACROMAN_H */
