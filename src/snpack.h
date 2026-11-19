// snpack.h - SNLZ: small dependency-free LZ77 byte codec for the shader / pipeline caches (v1.1.7).
// Pure C++17, no Windows headers (like logic.h / camcore.h) -> unit-tested on Linux (tests/test_snpack.cpp).
//
// Why
//   SN_DLSSG_psocache.bin holds the driver's compiled pipeline blobs of the GAME (hundreds of MB for a big UE5 game) and the same blobs live in RAM
//   (psocachemb).  GPU machine code, DXBC and driver metadata are full of repeated instruction patterns, so a plain LZ pass shrinks them a lot:
//   the same file / the same RAM budget now holds more pipelines (= fewer shader-compile stutters), or the same pipelines take less space.
//
// Design goals (in this order)
//   1. The DECODER is strict and can never read / write out of bounds, whatever bytes it gets (corrupt file, bit rot, attack): every length is checked,
//      the result must be EXACTLY rawSize bytes.  It runs on the game's render / RHI thread when a cached pipeline is requested -> it is fast (memcpy based).
//   2. The ENCODER only runs on our background appender thread (below-normal priority) -> it can afford hash chains + lazy matching.
//   3. Never make things bigger: Pack() returns 0 when the data is not smaller; the caller then stores the bytes raw (method 0).
//
// Stream format (byte oriented, LZ4-like, plus a "repeat offset" flag)
//   sequence := token  [extra literal length bytes]  literals  [ [offset(2 bytes LE)]  [extra match length bytes] ]
//   token    := REP (1 bit, 0x80) | literalCount (3 bits, 0x70) | matchLength - 4 (4 bits, 0x0F)
//               literalCount 7 / matchLength nibble 15 mean "more bytes follow" (a run of 255 bytes continues, LZ4 style)
//   REP = 1 : the match uses the SAME distance as the previous match and the 2 offset bytes are left out (fixed-size instruction records in GPU code
//             and structured DXBC repeat the same distance over and over, so this saves 2 bytes per match);  REP = 0 : offset 1..65535 follows.
//   The last sequence has literals only (REP = 0): the stream simply ends right after its literals.  A stream may also end right after a match.
//   Minimum match length 4, window 64 KB.  Little-endian only (x86 / x64 - the only target of this project).
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace snpack {

static const size_t kMinMatch  = 4;
static const size_t kMaxOffset = 65535;
static const unsigned kHashBits = 16;

enum Method : uint8_t { kStored = 0, kSnLz = 1 };      // value kept in the cache records (never renumber)

inline size_t Bound(size_t n) { return n + n / 255 + 32; }     // buffer size that is always enough for Pack()

inline uint32_t Rd32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
inline uint32_t Hash4(uint32_t v) { return (v * 2654435761u) >> (32 - kHashBits); }
inline size_t MatchLen(const uint8_t* a, const uint8_t* b, size_t maxLen) {
    size_t l = 0;
    while (l + 8 <= maxLen) {
        uint64_t x, y; memcpy(&x, a + l, 8); memcpy(&y, b + l, 8);
        if (x != y) { while (a[l] == b[l]) l++; return l; }       // at least one byte of this 8-byte word differs -> the loop stops inside it
        l += 8;
    }
    while (l < maxLen && a[l] == b[l]) l++;
    return l;
}

// ------------------------------------------------------------------------------------------------ encoder
// Keeps its two work tables between calls (the appender thread packs thousands of blobs - no 512 KB allocation per blob).  One Packer per thread.
class Packer {
public:
    // Returns the packed size, or 0 when the data is not smaller than the input (caller stores it raw), is too small to matter, or does not fit into cap.
    // depth = how many hash-chain candidates are tried per position (8 = fast, 24 = default, 96 = best ratio).
    size_t Pack(const uint8_t* src, size_t n, uint8_t* dst, size_t cap, int depth = 24) {
        if (!src || !dst || n < 32 || n > 0x7FFFFFFFu) return 0;
        if (depth < 1) depth = 1;
        size_t limit = n - 1; if (cap < limit) limit = cap;           // output must stay below n, otherwise storing raw is better
        head_.assign((size_t)1 << kHashBits, -1); prev_.assign(kMaxOffset + 1, -1);
        src_ = src; n_ = n; depth_ = depth; rep_ = 0;
        uint8_t* op = dst; uint8_t* const oend = dst + limit;
        const size_t last = n - kMinMatch;                              // last position with 4 readable bytes
        size_t pos = 0, anchor = 0, len = 0, off = 0; bool have = false;
        while (pos <= last) {
            if (!have) Find(pos, len, off);
            have = false;
            Insert(pos);
            if (len < kMinMatch) { pos++; continue; }
            if (pos + 1 <= last) {                                      // lazy matching: is the match that starts one byte later clearly better?
                size_t len2, off2; Find(pos + 1, len2, off2);
                if (len2 >= kMinMatch && Score(len2, off2) > Score(len, off) + 1) { pos++; len = len2; off = off2; have = true; continue; }
            }
            if (!Emit(op, oend, src + anchor, pos - anchor, len, off, off == rep_)) return 0;
            rep_ = off;
            const size_t mend = pos + len;
            for (size_t q = pos + 1; q < mend && q <= last; q++) Insert(q);
            pos = mend; anchor = mend;
        }
        if (anchor < n && !Emit(op, oend, src + anchor, n - anchor, 0, 0, false)) return 0;
        return (size_t)(op - dst);
    }
private:
    std::vector<int32_t> head_, prev_;
    const uint8_t* src_ = nullptr; size_t n_ = 0, rep_ = 0; int depth_ = 24;

    void Insert(size_t p) { uint32_t h = Hash4(Rd32(src_ + p)); prev_[p & kMaxOffset] = head_[h]; head_[h] = (int32_t)p; }
    long Score(size_t len, size_t off) const { return (long)len - (off == rep_ ? 0 : 2); }     // a repeat offset saves the 2 offset bytes
    void Find(size_t p, size_t& bestLen, size_t& bestOff) const {
        bestLen = 0; bestOff = 0; if (p + kMinMatch > n_) return;
        const size_t maxLen = n_ - p; const uint8_t* cur = src_ + p; long bestScore = -1000000;
        if (rep_ && p >= rep_ && Rd32(cur) == Rd32(cur - rep_)) {
            size_t l = MatchLen(cur - rep_, cur, maxLen);
            if (l >= kMinMatch) { bestLen = l; bestOff = rep_; bestScore = (long)l; }
        }
        int32_t cand = head_[Hash4(Rd32(cur))]; int d = depth_;
        while (cand >= 0 && d-- > 0) {
            size_t off = p - (size_t)cand; if (off == 0 || off > kMaxOffset) break;
            if (off != rep_ && (bestLen == 0 || (bestLen < maxLen && src_[cand + bestLen] == cur[bestLen])) && Rd32(src_ + cand) == Rd32(cur)) {
                size_t l = MatchLen(src_ + cand, cur, maxLen);
                long sc = (long)l - 2;
                if (l >= kMinMatch && sc > bestScore) { bestScore = sc; bestLen = l; bestOff = off; if (l == maxLen) break; }
            }
            int32_t nx = prev_[(size_t)cand & kMaxOffset];
            if (nx >= cand) break;                                      // the ring buffer slot was overwritten by a newer position: chain ends here
            cand = nx;
        }
    }
    // offset = 0 together with matchLen > 0 is not allowed; useRep = reuse the previous distance (no offset bytes)
    static bool Emit(uint8_t*& op, uint8_t* oend, const uint8_t* lit, size_t litLen, size_t matchLen, size_t offset, bool useRep) {
        if ((size_t)(oend - op) < 1 + litLen + litLen / 255 + 1 + 2 + matchLen / 255 + 1) return false;     // conservative: never overruns, a borderline case just stays raw
        uint8_t* tok = op++;
        unsigned lc = litLen < 7 ? (unsigned)litLen : 7u, mc = 0;
        if (matchLen) { size_t m = matchLen - kMinMatch; mc = m < 15 ? (unsigned)m : 15u; }
        *tok = (uint8_t)((matchLen && useRep ? 0x80u : 0u) | lc << 4 | mc);
        if (lc == 7) { size_t r = litLen - 7; while (r >= 255) { *op++ = 255; r -= 255; } *op++ = (uint8_t)r; }
        memcpy(op, lit, litLen); op += litLen;
        if (!matchLen) return true;
        if (!useRep) { *op++ = (uint8_t)(offset & 0xFF); *op++ = (uint8_t)(offset >> 8); }
        if (mc == 15) { size_t r = matchLen - kMinMatch - 15; while (r >= 255) { *op++ = 255; r -= 255; } *op++ = (uint8_t)r; }
        return true;
    }
};

// ------------------------------------------------------------------------------------------------ decoder
// Decodes exactly rawLen bytes into dst.  Returns false (and the content of dst must be ignored) for ANY inconsistency.
inline bool Unpack(const uint8_t* src, size_t srcLen, uint8_t* dst, size_t rawLen) {
    if (!src || !dst || !srcLen || !rawLen) return false;
    const uint8_t* sp = src; const uint8_t* const send = src + srcLen;
    uint8_t* dp = dst; uint8_t* const dend = dst + rawLen; size_t rep = 0;
    while (sp < send) {
        const unsigned tok = *sp++;
        size_t lit = (tok >> 4) & 7u;
        if (lit == 7) { for (;;) { if (sp >= send) return false; unsigned b = *sp++; lit += b; if (lit > rawLen) return false; if (b != 255) break; } }
        if ((size_t)(send - sp) < lit || (size_t)(dend - dp) < lit) return false;
        memcpy(dp, sp, lit); sp += lit; dp += lit;
        if (sp == send && !(tok & 0x80u)) break;                         // final literals-only sequence (a REP match needs no more input bytes, so it may end right here)
        size_t off;
        if (tok & 0x80u) off = rep;                                      // repeat offset: no offset bytes
        else { if (send - sp < 2) return false; off = (size_t)sp[0] | ((size_t)sp[1] << 8); sp += 2; }
        if (off == 0 || off > (size_t)(dp - dst)) return false;
        rep = off;
        size_t ml = tok & 15u;
        if (ml == 15) { for (;;) { if (sp >= send) return false; unsigned b = *sp++; ml += b; if (ml > rawLen) return false; if (b != 255) break; } }
        ml += kMinMatch;
        if ((size_t)(dend - dp) < ml) return false;
        const uint8_t* m = dp - off;
        if (off >= ml) memcpy(dp, m, ml); else for (size_t i = 0; i < ml; i++) dp[i] = m[i];     // overlapping copy = run / short period pattern
        dp += ml;
    }
    return dp == dend;
}

}  // namespace snpack
