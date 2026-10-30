// android_compat.h: what FidelityFX SDK 1.1.4 (written for Windows) takes from MSVC, for the Android NDK build
// (forced in with -include by ffx_android.sh)
#pragma once
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#ifndef _countof
#define _countof(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifdef __cplusplus
#include <cstdlib>
#include <string>
static inline int wcscpy_s(wchar_t *d, size_t n, const wchar_t *s) { if (!n) return 1; wcsncpy(d, s, n - 1); d[n - 1] = 0; return 0; }
static inline int strcpy_s(char *d, size_t n, const char *s) { if (!n) return 1; strncpy(d, s, n - 1); d[n - 1] = 0; return 0; }
static inline int wcscat_s(wchar_t *d, size_t n, const wchar_t *s) { size_t l = wcslen(d); return l < n ? wcscpy_s(d + l, n - l, s) : 1; }
static inline int strcat_s(char *d, size_t n, const char *s) { size_t l = strlen(d); return l < n ? strcpy_s(d + l, n - l, s) : 1; }
template <size_t N> static inline int wcscpy_s(wchar_t (&d)[N], const wchar_t *s) { return wcscpy_s(d, N, s); }
#include <cstdio>
template <size_t N, typename... A> static inline int sprintf_s(char (&d)[N], const char *f, A... a) { return snprintf(d, N, f, a...); }
template <typename... A> static inline int sprintf_s(char *d, size_t n, const char *f, A... a) { return snprintf(d, n, f, a...); }
template <size_t N, typename... A> static inline int swprintf_s(wchar_t (&d)[N], const wchar_t *f, A... a) { return swprintf(d, N, f, a...); }
template <typename... A> static inline int swprintf_s(wchar_t *d, size_t n, const wchar_t *f, A... a) { return swprintf(d, n, f, a...); }
#ifndef FFX_UNUSED
#define FFX_UNUSED(x) ((void)(x))
#endif
template <size_t N> static inline int strcpy_s(char (&d)[N], const char *s) { return strcpy_s(d, N, s); }
// std::wstring_convert is gone from the NDK's libc++: UTF-8 <-> wchar_t (UTF-32 on Android) by hand
namespace ffx_android {
static inline std::wstring from_utf8(const char *s) {
    std::wstring w;
    for (const unsigned char *p = (const unsigned char *)s; *p;) {
        unsigned c = *p++;
        int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= n == 3 ? 7 : n == 2 ? 15 : n == 1 ? 31 : 127;
        while (n-- && (*p & 0xC0) == 0x80) c = (c << 6) | (*p++ & 63);
        w.push_back((wchar_t)c);
    }
    return w;
}
static inline std::string to_utf8(const wchar_t *w) {
    std::string s;
    for (; *w; w++) {
        unsigned c = (unsigned)*w;
        if (c < 0x80) s.push_back((char)c);
        else if (c < 0x800) s.push_back((char)(0xC0 | c >> 6)), s.push_back((char)(0x80 | (c & 63)));
        else if (c < 0x10000) s.push_back((char)(0xE0 | c >> 12)), s.push_back((char)(0x80 | (c >> 6 & 63))), s.push_back((char)(0x80 | (c & 63)));
        else s.push_back((char)(0xF0 | c >> 18)), s.push_back((char)(0x80 | (c >> 12 & 63))), s.push_back((char)(0x80 | (c >> 6 & 63))), s.push_back((char)(0x80 | (c & 63)));
    }
    return s;
}
}  // namespace ffx_android
#endif
