// String Manager HLE and a Palm-compatible printf.
#include "palmos.h"
#include <cctype>
#include <functional>
#include <memory>

// Palm StrPrintF subset: %[-+ 0#][width][.prec][h|l|ll]{d,i,u,x,X,o,s,c,p,%}
std::string palm_format(const std::string& fmt, const std::function<uint32_t()>& next) {
    std::string out;
    for (size_t i = 0; i < fmt.size(); i++) {
        char c = fmt[i];
        if (c != '%') { out += c; continue; }
        if (++i >= fmt.size()) break;
        if (fmt[i] == '%') { out += '%'; continue; }
        std::string spec = "%";
        while (i < fmt.size() && strchr("-+ 0#", fmt[i])) spec += fmt[i++];
        while (i < fmt.size() && (isdigit((unsigned char)fmt[i]) || fmt[i] == '*')) {
            if (fmt[i] == '*') spec += std::to_string((int32_t)next()); else spec += fmt[i];
            i++;
        }
        if (i < fmt.size() && fmt[i] == '.') {
            spec += fmt[i++];
            while (i < fmt.size() && (isdigit((unsigned char)fmt[i]) || fmt[i] == '*')) {
                if (fmt[i] == '*') spec += std::to_string((int32_t)next()); else spec += fmt[i];
                i++;
            }
        }
        int len = 0;   // 1 = h, 2 = l, 3 = ll
        while (i < fmt.size() && (fmt[i] == 'h' || fmt[i] == 'l')) { len = fmt[i] == 'h' ? 1 : len + 2 > 3 ? 3 : len + 2; i++; }
        if (i >= fmt.size()) break;
        char conv = fmt[i];
        char buf[512];
        switch (conv) {
            case 'd': case 'i': {
                long long v;
                if (len == 3) { uint32_t lo = next(), hi = next(); v = (long long)((uint64_t)hi << 32 | lo); }
                else v = len == 1 ? (int16_t)next() : (int32_t)next();
                snprintf(buf, sizeof buf, (spec + "lld").c_str(), v);
                break;
            }
            case 'u': case 'x': case 'X': case 'o': {
                unsigned long long v;
                if (len == 3) { uint32_t lo = next(), hi = next(); v = (uint64_t)hi << 32 | lo; }
                else v = len == 1 ? (uint16_t)next() : next();
                snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), v);
                break;
            }
            case 'c': snprintf(buf, sizeof buf, (spec + "c").c_str(), (char)next()); break;
            case 'p': snprintf(buf, sizeof buf, "%08x", next()); break;
            case 's': {
                std::string s = rstr(next(), 4096);
                snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
                break;
            }
            case 'f': case 'g': case 'e': {   // doubles arrive as two words (soft-float, little endian)
                uint32_t lo = next(), hi = next();
                uint64_t bits = (uint64_t)hi << 32 | lo;
                double d;
                memcpy(&d, &bits, 8);
                snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
                break;
            }
            default: buf[0] = conv; buf[1] = 0;
        }
        out += buf;
    }
    return out;
}

static std::function<uint32_t()> arg_reader(Args& a, int first) {
    auto idx = std::make_shared<int>(first);
    return [&a, idx]() { return a[(*idx)++]; };
}
static std::function<uint32_t()> va_reader(uint32_t va) {
    auto p = std::make_shared<uint32_t>(va);
    return [p]() { uint32_t v = r32(*p); *p += 4; return v; };
}

HLE(StrPrintF) {
    std::string s = palm_format(rstr(a[1]), arg_reader(a, 2));
    wstr(a[0], s);
    return (uint32_t)s.size();
}
HLE(StrVPrintF) {
    std::string s = palm_format(rstr(a[1]), va_reader(a[2]));
    wstr(a[0], s);
    return (uint32_t)s.size();
}
HLE(dprintf) {
    std::string s = palm_format(rstr(a[0]), arg_reader(a, 1));
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    LOG("[dprintf] %s", s.c_str());
    return 0;
}

HLE(StrCopy) { wstr(a[0], rstr(a[1], 1 << 20)); return a[0]; }
HLE(StrNCopy) {
    // Palm StrNCopy: copies up to n chars, pads with NULs if src is shorter
    int16_t n = (int16_t)a[2];
    std::string s = rstr(a[1], n > 0 ? n : 0);
    for (int i = 0; i < n; i++) w8(a[0] + i, i < (int)s.size() ? (uint8_t)s[i] : 0);
    return a[0];
}
HLE(StrCat) { std::string d = rstr(a[0], 1 << 20); wstr(a[0] + (uint32_t)d.size(), rstr(a[1], 1 << 20)); return a[0]; }
HLE(StrNCat) {
    // n is the max total size of dst including the terminator
    int16_t n = (int16_t)a[2];
    std::string d = rstr(a[0], 1 << 20), s = rstr(a[1], 1 << 20);
    int room = n - 1 - (int)d.size();
    if (room > 0) wstr(a[0] + (uint32_t)d.size(), s.substr(0, room));
    return a[0];
}
HLE(StrLen) { return (uint32_t)rstr(a[0], 1 << 20).size(); }
static int sgn(int v) { return v < 0 ? -1 : v > 0 ? 1 : 0; }
HLE(StrCompare) { return (uint32_t)sgn(strcmp(rstr(a[0], 1 << 20).c_str(), rstr(a[1], 1 << 20).c_str())); }
HLE(StrCompareAscii) { return (uint32_t)sgn(strcmp(rstr(a[0], 1 << 20).c_str(), rstr(a[1], 1 << 20).c_str())); }
HLE(StrNCompare) { return (uint32_t)sgn(strncmp(rstr(a[0], 1 << 20).c_str(), rstr(a[1], 1 << 20).c_str(), a[2])); }
HLE(StrNCompareAscii) { return (uint32_t)sgn(strncmp(rstr(a[0], 1 << 20).c_str(), rstr(a[1], 1 << 20).c_str(), a[2])); }
HLE(StrCaselessCompare) { return (uint32_t)sgn(_stricmp(rstr(a[0], 1 << 20).c_str(), rstr(a[1], 1 << 20).c_str())); }
HLE(StrNCaselessCompare) { return (uint32_t)sgn(_strnicmp(rstr(a[0], 1 << 20).c_str(), rstr(a[1], 1 << 20).c_str(), a[2])); }
HLE(StrToLower) {
    std::string s = rstr(a[1], 1 << 20);
    for (auto& ch : s) ch = (char)tolower((unsigned char)ch);
    wstr(a[0], s);
    return a[0];
}
HLE(StrIToA) { wstr(a[0], std::to_string((int32_t)a[1])); return a[0]; }
HLE(StrIToH) { char b[16]; snprintf(b, sizeof b, "%08X", a[1]); wstr(a[0], b); return a[0]; }
HLE(StrAToI) { return (uint32_t)atoi(rstr(a[0], 64).c_str()); }
HLE(StrChr) {
    std::string s = rstr(a[0], 1 << 20);
    size_t p = s.find((char)a[1]);
    return p == std::string::npos ? 0 : a[0] + (uint32_t)p;
}
HLE(StrStr) {
    std::string s = rstr(a[0], 1 << 20), t = rstr(a[1], 1 << 20);
    size_t p = s.find(t);
    return p == std::string::npos ? 0 : a[0] + (uint32_t)p;
}
HLE(StrLocalizeNumber) { return a[0]; }
HLE(StrDelocalizeNumber) { return a[0]; }
