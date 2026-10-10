// logic.h (v27) - platform independent helpers: NO windows.h, NO globals. Everything here is unit-tested on the build machine (tests/test_logic.cpp).
//   presets, output-fps cap maths, HUD-less auto-detection decision, anti-cheat name lists, UE version string scan, ZIP (store) writer, user-name redaction.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <map>

// ------------------------------------------------------------------ presets
// A preset only sets these four keys (everything else stays as the user has it). In the config file the preset is applied FIRST, so single keys written
// below "preset=" still win.   quality = pause FG early (fewer artefacts) | balanced = the shipped defaults | performance = never pause, highest multiplier the GPU supports
struct SnPreset { int fgMinFps, fgDelay, camStale, mult; };
enum { SN_PRESET_CUSTOM = 0, SN_PRESET_QUALITY = 1, SN_PRESET_BALANCED = 2, SN_PRESET_PERFORMANCE = 3, SN_PRESET_COUNT = 4 };
static inline std::string SnLower(const std::string& s) { std::string r = s; for (auto& c : r) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return r; }
static inline int SnPresetParse(const char* s) {
    std::string t; for (; s && *s && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n'; s++) t += *s; t = SnLower(t);
    if (t == "quality") return SN_PRESET_QUALITY; if (t == "balanced") return SN_PRESET_BALANCED; if (t == "performance") return SN_PRESET_PERFORMANCE;
    return SN_PRESET_CUSTOM;
}
static inline const char* SnPresetName(int id) { switch (id) { case SN_PRESET_QUALITY: return "quality"; case SN_PRESET_BALANCED: return "balanced"; case SN_PRESET_PERFORMANCE: return "performance"; default: return "custom"; } }
static inline bool SnPresetGet(int id, SnPreset* o) {
    switch (id) {
    case SN_PRESET_QUALITY:     *o = { 30, 45, 30, 2 }; return true;
    case SN_PRESET_BALANCED:    *o = { 24, 30, 30, 2 }; return true;
    case SN_PRESET_PERFORMANCE: *o = { 0, 15, 45, 4 }; return true;
    default: return false; }
}

// ------------------------------------------------------------------ output fps cap (keeps the OUTPUT fps inside the VRR / G-Sync window)
// fpscap = 0 off | -1 auto (display refresh - 3) | N fixed output fps.   Reflex limits the BASE fps, so the base limit is outCap / (frames per game frame).
static inline int SnAutoOutCap(int refreshHz) { return refreshHz < 24 ? 0 : std::max(20, refreshHz - 3); }
static inline int SnResolveOutCap(int cfgCap, int refreshHz) { return cfgCap < 0 ? SnAutoOutCap(refreshHz) : cfgCap; }
// returns the Reflex base-fps limit (0 = none). baseCfg = the user's own 'basefpslimit' (the smaller of the two wins)
static inline int SnBaseLimit(int outCap, int baseCfg, int mult, bool fgActive) {
    int base = 0;
    if (outCap > 0) { int m = fgActive ? std::max(1, mult) : 1; base = std::max(10, outCap / m); }
    if (baseCfg > 0 && (base == 0 || baseCfg < base)) base = baseCfg;
    return base;
}

// ------------------------------------------------------------------ HUD-less auto detection
// The injector samples a coarse grid of back-buffer pixels BEFORE each of the first draws that target the back buffer. changed[i] = fraction of grid points
// that draw number (i+1) changed. Scene passes (tonemap / upscale / post) cover the whole screen, UI draws only small parts. The HUD-less copy has to be taken
// right after the last full-screen pass = before draw number (lastFull + 2) => value for 'hudlessdraw'.   Returns 0 when this frame cannot tell.
static inline int SnHudDecideFrame(const double* changed, int n, double fullThresh = 0.6) {
    int i = 0; while (i < n && changed[i] < fullThresh) i++;
    if (i >= n) return 0;                                   // no full-screen pass seen (menu, static frame ...)
    int j = i; while (j < n && changed[j] >= fullThresh) j++;
    if (j >= n) return 0;                                   // the full-screen run reaches the end of the sampled window: cannot see where the UI starts
    return j + 1;                                           // draw numbers are 1-based; capture happens BEFORE draw number (j+1)
}
// decisions = the N found in several frames (only conclusive ones). Returns N when enough frames agree, else 0.
static inline int SnHudVote(const std::vector<int>& d, int minVotes, double minAgree, int* agreeOut = nullptr) {
    if ((int)d.size() < minVotes) return 0;
    int best = 0, bc = 0; for (int v : d) { int c = (int)std::count(d.begin(), d.end(), v); if (c > bc) { bc = c; best = v; } }
    if (agreeOut) *agreeOut = bc;
    return ((double)bc / (double)d.size() >= minAgree) ? best : 0;
}
enum { SN_PIX_NONE = 0, SN_PIX_U8x4 = 1, SN_PIX_R10G10B10A2 = 2, SN_PIX_F16x4 = 3 };
static inline float SnHalf(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023; float v;
    if (e == 0) v = std::ldexp((float)m, -24); else if (e == 31) v = m ? NAN : INFINITY; else v = std::ldexp((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
static inline bool SnPixDiffer(int cls, const uint8_t* a, const uint8_t* b) {
    if (cls == SN_PIX_U8x4) { for (int c = 0; c < 3; c++) if (abs((int)a[c] - (int)b[c]) > 3) return true; return false; }
    if (cls == SN_PIX_R10G10B10A2) { uint32_t x, y; memcpy(&x, a, 4); memcpy(&y, b, 4); for (int c = 0; c < 3; c++) { int p = (int)((x >> (10 * c)) & 1023), q = (int)((y >> (10 * c)) & 1023); if (abs(p - q) > 12) return true; } return false; }
    if (cls == SN_PIX_F16x4) { for (int c = 0; c < 3; c++) { uint16_t p, q; memcpy(&p, a + 2 * c, 2); memcpy(&q, b + 2 * c, 2); float fp = SnHalf(p), fq = SnHalf(q); if (!(std::isfinite(fp) && std::isfinite(fq))) { if (p != q) return true; continue; } if (std::fabs(fp - fq) > 0.01f + 0.02f * std::max(std::fabs(fp), std::fabs(fq))) return true; } return false; }
    return false;
}
// two sample sets (nPoints pixels, 'stride' bytes apart) -> fraction of points that differ
static inline double SnChangedFraction(int cls, const uint8_t* A, const uint8_t* B, int nPoints, size_t stride) {
    if (nPoints <= 0) return 0.0; int d = 0; for (int i = 0; i < nPoints; i++) if (SnPixDiffer(cls, A + (size_t)i * stride, B + (size_t)i * stride)) d++;
    return (double)d / (double)nPoints;
}

// ------------------------------------------------------------------ anti-cheat names (lower-case file / folder / module names)
// Returns the product name or nullptr. The injector switches itself OFF when one of these is found (anticheat=1, default) - so no account can be flagged by it.
static inline const char* SnAcName(const std::string& n) {
    auto pre = [&](const char* p) { return n.compare(0, strlen(p), p) == 0; };
    if (pre("easyanticheat") || n == "start_protected_game.exe" || pre("eosanticheat")) return "Easy Anti-Cheat";
    if (n == "battleye" || pre("beservice") || pre("beclient") || pre("bedaisy")) return "BattlEye";
    if (n == "gameguard" || n == "gameguard.des" || pre("npggnt") || (pre("npgg") && n.size() > 4 && n.compare(n.size() - 4, 4, ".des") == 0)) return "nProtect GameGuard";
    if (pre("xigncode") || n == "x3.xem") return "XIGNCODE3";
    if (n == "vgc.exe" || n == "vgk.sys") return "Riot Vanguard";
    if (pre("anticheatexpert") || pre("tersafe")) return "Tencent Anti-Cheat Expert";
    if (n == "pbsvc.exe" || n == "pbcl.dll") return "PunkBuster";
    return nullptr;
}

// ------------------------------------------------------------------ UE version string scan: "++UE4+Release-4.26" / "++UE5+Release-5.3" (ASCII or UTF-16) inside a memory block. 5, 4 or 0
static inline int SnScanUeVersion(const uint8_t* p, size_t n) {
    int found = 0;
    for (size_t i = 0; i + 6 <= n; i++) {
        const uint8_t* q = (const uint8_t*)memchr(p + i, '+', n - i); if (!q) break; i = (size_t)(q - p);
        if (i + 6 <= n && q[1] == '+' && q[2] == 'U' && q[3] == 'E' && (q[4] == '4' || q[4] == '5') && q[5] == '+') { found = std::max(found, q[4] - '0'); if (found == 5) return 5; }
        else if (i + 12 <= n && q[1] == 0 && q[2] == '+' && q[3] == 0 && q[4] == 'U' && q[5] == 0 && q[6] == 'E' && q[7] == 0 && (q[8] == '4' || q[8] == '5') && q[9] == 0 && q[10] == '+' && q[11] == 0) { found = std::max(found, q[8] - '0'); if (found == 5) return 5; }
    }
    return found;
}

// ------------------------------------------------------------------ ZIP writer (method 0 = stored; enough for text logs, no dependencies)
static inline uint32_t SnCrc32(const uint8_t* d, size_t n, uint32_t crc = 0) {
    static uint32_t t[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = true; }
    crc = ~crc; for (size_t i = 0; i < n; i++) crc = t[(crc ^ d[i]) & 255] ^ (crc >> 8); return ~crc;
}
struct SnZip {
    struct Ent { std::string name; uint32_t crc, size, off; };
    std::string out; std::vector<Ent> ents; uint16_t dosTime = 0, dosDate = (uint16_t)(((2026 - 1980) << 9) | (1 << 5) | 1);
    static void P16(std::string& s, uint32_t v) { s += (char)(v & 255); s += (char)((v >> 8) & 255); }
    static void P32(std::string& s, uint32_t v) { P16(s, v & 0xFFFF); P16(s, v >> 16); }
    void SetTime(int y, int mo, int d, int h, int mi, int sec) { dosDate = (uint16_t)(((y - 1980) << 9) | (mo << 5) | d); dosTime = (uint16_t)((h << 11) | (mi << 5) | (sec / 2)); }
    void Add(const std::string& name, const std::string& data) {
        Ent e{ name, SnCrc32((const uint8_t*)data.data(), data.size()), (uint32_t)data.size(), (uint32_t)out.size() };
        P32(out, 0x04034B50); P16(out, 20); P16(out, 0x0800); P16(out, 0); P16(out, dosTime); P16(out, dosDate); P32(out, e.crc); P32(out, e.size); P32(out, e.size); P16(out, (uint32_t)name.size()); P16(out, 0);
        out += name; out += data; ents.push_back(e);
    }
    std::string Finish() {
        uint32_t cdOff = (uint32_t)out.size();
        for (auto& e : ents) { P32(out, 0x02014B50); P16(out, 20); P16(out, 20); P16(out, 0x0800); P16(out, 0); P16(out, dosTime); P16(out, dosDate); P32(out, e.crc); P32(out, e.size); P32(out, e.size);
            P16(out, (uint32_t)e.name.size()); P16(out, 0); P16(out, 0); P16(out, 0); P16(out, 0); P32(out, 0); P32(out, e.off); out += e.name; }
        uint32_t cdSize = (uint32_t)out.size() - cdOff;
        P32(out, 0x06054B50); P16(out, 0); P16(out, 0); P16(out, (uint32_t)ents.size()); P16(out, (uint32_t)ents.size()); P32(out, cdSize); P32(out, cdOff); P16(out, 0);
        return out;
    }
};

// ------------------------------------------------------------------ privacy helpers for the support bundle
// "C:\Users\Alice\Games\x" -> "C:\Users\<user>\Games\x"  (case-insensitive, both slash types)
static inline std::string SnRedactUser(const std::string& text, const std::string& user) {
    if (user.empty()) return text; std::string low = SnLower(text), lu = SnLower(user), out; size_t pos = 0;
    while (pos < text.size()) {
        size_t f = low.find(lu, pos); if (f == std::string::npos) { out += text.substr(pos); break; }
        bool pre = f >= 6 && low.compare(f - 6, 6, "users\\") == 0; bool pre2 = f >= 6 && low.compare(f - 6, 6, "users/") == 0;
        size_t e = f + lu.size(); bool post = e >= text.size() || text[e] == '\\' || text[e] == '/' || text[e] == '"' || text[e] == ' ' || text[e] == '\r' || text[e] == '\n';
        out += text.substr(pos, f - pos); if ((pre || pre2) && post) out += "<user>"; else out += text.substr(f, lu.size()); pos = e;
    }
    return out;
}
// last maxBytes of s, starting at a line boundary
static inline std::string SnTail(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s; size_t st = s.size() - maxBytes; size_t nl = s.find('\n', st); if (nl != std::string::npos && nl + 1 < s.size()) st = nl + 1; return "[... older part cut ...]\n" + s.substr(st);
}

// ------------------------------------------------------------------ config file rewrite
// config text transformation (cfgsave.inc): replaces "key=value" lines in place, keeps comments / unknown keys / order, appends keys that are not in the file yet. Returns the new content.
static inline std::string SnCfgApplyChanges(const std::string& in, const std::map<std::string, std::string>& changes) {
    std::string eol = in.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::map<std::string, std::string> left = changes;
    std::string out; out.reserve(in.size() + 256);
    size_t pos = 0;
    while (pos < in.size()) {
        size_t e = in.find('\n', pos); bool hadNl = e != std::string::npos; if (!hadNl) e = in.size();
        std::string line = in.substr(pos, e - pos); if (!line.empty() && line.back() == '\r') line.pop_back();
        pos = hadNl ? e + 1 : e;
        bool done = false;
        size_t i = 0; while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i < line.size() && line[i] != '#' && line[i] != ';') {
            size_t eq = line.find('=', i);
            if (eq != std::string::npos) {
                std::string key = line.substr(i, eq - i); while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
                auto it = changes.find(key);
                if (it != changes.end()) { out += key + "=" + it->second; left.erase(key); done = true; }
            }
        }
        if (!done) out += line;
        if (hadNl || pos < in.size()) out += eol;
    }
    if (!out.empty() && out.back() != '\n') out += eol;
    if (!left.empty()) {
        out += "# --- written by the in-game menu ---" + eol;
        for (auto& kv : left) out += kv.first + "=" + kv.second + eol;
    }
    return out;
}

