// camtrace.h - SNCT v1: compact frame-sequence trace of what the camera tracker is FED (pure C++17, no windows.h, no globals).
// Used by: (a) the injector's event-window recorder (later), (b) tests/replay_seq.cpp (step 3), (c) synthetic scenario generators.
//
// Unit of recording = one CALL of ParseViewBuffer (NOT one frame): alternating views / several view buffers per frame are exactly the
// cases that broke in 1.1.6, so the trace must keep each buffer the tracker was shown, in order.
//
// File = HEADER (96 bytes) + records. Record = u8 type, u32 payloadBytes, payload. Unknown types are skipped (forward compatible).
// All integers little-endian. Floats are IEEE-754 binary32.
//
//   REC_CALL   i32 frame, u16 effW, u16 effH, u32 bufId, u16 availF, u16 nBlocks, nBlocks x { u16 floatIdx, f32 m[16] }
//   REC_EXPECT i32 frame, u16 event, u16 pad, i32 a0, i32 a1        (assertion for the replayer: "this event fires at this frame")
//   REC_MARK   i32 frame, u16 code, u16 pad                         (why the recorder flushed a window; informational)
//
// A CALL does not need the whole buffer. Blocks are 16-float windows; every float NOT covered by a block replays as 0.0f
// (Materialize). A sparse recording is only valid if replaying it gives the same decisions as the full buffer - step 3 checks this
// on every full dump we have (FLAG_SPARSE traces must be derived from full ones in the tests, never trusted blindly).
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace snct {

enum : uint16_t {   // events: written by synthetic scenarios as EXPECT records, produced by CamTracker::Step later
    EV_NONE = 0, EV_LOCKED = 1, EV_SWITCHED = 2, EV_CUT = 3, EV_LOST = 4, EV_CAND_IGNORED = 5,
    EV_ALT_VIEW = 6, EV_HOLD = 7, EV_RESCAN = 8, EV_GARBAGE = 9, EV_GARBAGE_CLEARED = 10
};
enum : uint8_t  { REC_CALL = 1, REC_EXPECT = 2, REC_MARK = 3 };
enum : uint32_t { FLAG_SPARSE = 1, FLAG_EVENT_WINDOW = 2 };

static const uint16_t kVersion = 1;
static const uint16_t kHeaderBytes = 96;

// Everything the tracker reads from g_cfg (the 11 ints of the shim) + what was locked when the capture began. A failing test is self-describing.
struct Header {
    uint32_t flags = 0;
    char     tag[24] = {};                    // injector version / game id, NUL padded
    int32_t  startFrame = 0;
    int32_t  projOff = -1, noAAOff = -1, mvOff = -1;   // offsets in force at the first record (-1 = none yet)
    int32_t  c2pIdx = 0, camAuto = 1, c2pAuto = 1, autoLearn = 1, cutDetect = 1, cutFov = 8, jitSX = 1, jitSY = 1;
};
struct Block { uint16_t idx = 0; float m[16] = {}; };
struct Call  { int32_t frame = 0; uint16_t effW = 0, effH = 0; uint32_t bufId = 0; uint16_t availF = 0; std::vector<Block> blocks; };
struct Expect{ int32_t frame = 0; uint16_t ev = EV_NONE; int32_t a0 = 0, a1 = 0; };
struct Item  { uint8_t type = 0; Call call; Expect ex; int32_t markFrame = 0; uint16_t markCode = 0; };
struct Trace { Header h; std::vector<Item> items; };

// The buffer the tracker is handed: availF floats, zero except for the recorded blocks.
static inline void Materialize(const Call& c, std::vector<float>& out) {
    out.assign(c.availF, 0.f);
    for (const Block& b : c.blocks) for (int i = 0; i < 16; i++) if ((size_t)b.idx + i < out.size()) out[b.idx + i] = b.m[i];
}

namespace detail {
struct Buf {
    std::vector<uint8_t> d;
    void u8(uint8_t v) { d.push_back(v); }
    void u16(uint16_t v) { u8((uint8_t)(v & 255)); u8((uint8_t)(v >> 8)); }
    void u32(uint32_t v) { u16((uint16_t)(v & 0xFFFF)); u16((uint16_t)(v >> 16)); }
    void i32(int32_t v) { u32((uint32_t)v); }
    void f32(float f) { uint32_t u; memcpy(&u, &f, 4); u32(u); }
    void raw(const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; d.insert(d.end(), b, b + n); }
};
struct Cur {
    const uint8_t* p; const uint8_t* e; bool ok = true;
    Cur(const uint8_t* a, const uint8_t* b) : p(a), e(b) {}
    bool need(size_t n) { if ((size_t)(e - p) < n) { ok = false; return false; } return true; }
    uint8_t  u8()  { if (!need(1)) return 0; return *p++; }
    uint16_t u16() { uint16_t a = u8(); a = (uint16_t)(a | ((uint16_t)u8() << 8)); return a; }
    uint32_t u32() { uint32_t a = u16(); a |= (uint32_t)u16() << 16; return a; }
    int32_t  i32() { return (int32_t)u32(); }
    float    f32() { uint32_t u = u32(); float f; memcpy(&f, &u, 4); return f; }
};
}  // namespace detail

class Writer {
public:
    void Begin(const Header& h) {
        b_.d.clear();
        b_.raw("SNCT", 4); b_.u16(kVersion); b_.u16(kHeaderBytes); b_.u32(h.flags);
        char tag[24] = {}; memcpy(tag, h.tag, sizeof tag); tag[23] = 0; b_.raw(tag, 24);
        b_.i32(h.startFrame); b_.i32(h.projOff); b_.i32(h.noAAOff); b_.i32(h.mvOff);
        b_.i32(h.c2pIdx); b_.i32(h.camAuto); b_.i32(h.c2pAuto); b_.i32(h.autoLearn); b_.i32(h.cutDetect); b_.i32(h.cutFov); b_.i32(h.jitSX); b_.i32(h.jitSY);
        while (b_.d.size() < kHeaderBytes) b_.u8(0);
    }
    void AddCall(const Call& c) {
        detail::Buf p; p.i32(c.frame); p.u16(c.effW); p.u16(c.effH); p.u32(c.bufId); p.u16(c.availF); p.u16((uint16_t)c.blocks.size());
        for (const Block& k : c.blocks) { p.u16(k.idx); for (int i = 0; i < 16; i++) p.f32(k.m[i]); }
        Rec(REC_CALL, p);
    }
    void AddExpect(const Expect& e) { detail::Buf p; p.i32(e.frame); p.u16(e.ev); p.u16(0); p.i32(e.a0); p.i32(e.a1); Rec(REC_EXPECT, p); }
    void AddMark(int32_t frame, uint16_t code) { detail::Buf p; p.i32(frame); p.u16(code); p.u16(0); Rec(REC_MARK, p); }
    const std::vector<uint8_t>& Bytes() const { return b_.d; }
    bool Save(const char* path) const {
        FILE* f = fopen(path, "wb"); if (!f) return false;
        bool ok = fwrite(b_.d.data(), 1, b_.d.size(), f) == b_.d.size(); fclose(f); return ok;
    }
private:
    void Rec(uint8_t type, const detail::Buf& p) { b_.u8(type); b_.u32((uint32_t)p.d.size()); b_.d.insert(b_.d.end(), p.d.begin(), p.d.end()); }
    detail::Buf b_;
};

// Strict, bounds-checked. Returns false (and a reason) on a bad magic / version / truncated or malformed record; never reads out of range.
static inline bool Parse(const uint8_t* data, size_t n, Trace& out, std::string* err = nullptr) {
    auto fail = [&](const char* m) { if (err) *err = m; return false; };
    out = Trace();
    if (n < kHeaderBytes) return fail("short header");
    if (memcmp(data, "SNCT", 4)) return fail("bad magic");
    detail::Cur c(data + 4, data + n);
    uint16_t ver = c.u16(), hb = c.u16();
    if (ver != kVersion) return fail("unsupported version");
    if (hb < kHeaderBytes || hb > n) return fail("bad header size");
    Header& h = out.h; h.flags = c.u32();
    memcpy(h.tag, c.p, 24); c.p += 24; h.tag[23] = 0;
    h.startFrame = c.i32(); h.projOff = c.i32(); h.noAAOff = c.i32(); h.mvOff = c.i32();
    h.c2pIdx = c.i32(); h.camAuto = c.i32(); h.c2pAuto = c.i32(); h.autoLearn = c.i32(); h.cutDetect = c.i32(); h.cutFov = c.i32(); h.jitSX = c.i32(); h.jitSY = c.i32();
    const uint8_t* q = data + hb; const uint8_t* end = data + n;
    while (q < end) {
        detail::Cur r(q, end); uint8_t type = r.u8(); uint32_t len = r.u32();
        if (!r.ok || (size_t)(end - r.p) < len) return fail("truncated record");
        detail::Cur p(r.p, r.p + len); q = r.p + len;
        Item it; it.type = type;
        if (type == REC_CALL) {
            Call& k = it.call; k.frame = p.i32(); k.effW = p.u16(); k.effH = p.u16(); k.bufId = p.u32(); k.availF = p.u16(); uint16_t nb = p.u16();
            if (!p.ok || (size_t)(p.e - p.p) != (size_t)nb * 66) return fail("malformed call record");
            k.blocks.resize(nb);
            for (uint16_t i = 0; i < nb; i++) { k.blocks[i].idx = p.u16(); for (int j = 0; j < 16; j++) k.blocks[i].m[j] = p.f32(); }
        } else if (type == REC_EXPECT) {
            it.ex.frame = p.i32(); it.ex.ev = p.u16(); p.u16(); it.ex.a0 = p.i32(); it.ex.a1 = p.i32();
            if (!p.ok) return fail("malformed expect record");
        } else if (type == REC_MARK) {
            it.markFrame = p.i32(); it.markCode = p.u16(); p.u16();
            if (!p.ok) return fail("malformed mark record");
        } else continue;   // unknown type: skipped (forward compatible)
        out.items.push_back(std::move(it));
    }
    return true;
}
static inline bool Load(const char* path, Trace& out, std::string* err = nullptr) {
    FILE* f = fopen(path, "rb"); if (!f) { if (err) *err = "cannot open file"; return false; }
    std::vector<uint8_t> d; uint8_t tmp[65536]; size_t k;
    while ((k = fread(tmp, 1, sizeof tmp, f)) > 0) d.insert(d.end(), tmp, tmp + k);
    fclose(f); return Parse(d.data(), d.size(), out, err);
}

// Human-readable dump (diff-friendly; used when a test fails and for reviewing a captured window).
static inline void DumpText(FILE* f, const Trace& t) {
    const Header& h = t.h;
    fprintf(f, "# SNCT v%u tag=%s flags=0x%x start=%d locked proj/noaa/mv=%d/%d/%d cfg c2pIdx=%d camAuto=%d c2pAuto=%d autoLearn=%d cutDetect=%d cutFov=%d jit=%d,%d\n",
            kVersion, h.tag, h.flags, h.startFrame, h.projOff, h.noAAOff, h.mvOff, h.c2pIdx, h.camAuto, h.c2pAuto, h.autoLearn, h.cutDetect, h.cutFov, h.jitSX, h.jitSY);
    for (const Item& it : t.items) {
        if (it.type == REC_CALL) {
            fprintf(f, "CALL frame=%d view=%ux%u buf=%08x avail=%u blocks=%zu\n", it.call.frame, it.call.effW, it.call.effH, it.call.bufId, it.call.availF, it.call.blocks.size());
            for (const Block& b : it.call.blocks) fprintf(f, "  @%-5u %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g\n", b.idx,
                b.m[0], b.m[1], b.m[2], b.m[3], b.m[4], b.m[5], b.m[6], b.m[7], b.m[8], b.m[9], b.m[10], b.m[11], b.m[12], b.m[13], b.m[14], b.m[15]);
        } else if (it.type == REC_EXPECT) fprintf(f, "EXPECT frame=%d event=%u a0=%d a1=%d\n", it.ex.frame, it.ex.ev, it.ex.a0, it.ex.a1);
        else if (it.type == REC_MARK) fprintf(f, "MARK frame=%d code=%u\n", it.markFrame, it.markCode);
    }
}

}  // namespace snct
