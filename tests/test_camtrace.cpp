// tests/test_camtrace.cpp - SNCT v1 format: round trip, sparse materialize, unknown-record skip, truncation / fuzz safety.
#include "camtrace.h"
#include <cstdlib>
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
using namespace snct;

static Call MakeCall(int fr, uint32_t buf, float m0) {
    Call c; c.frame = fr; c.effW = 1400; c.effH = 788; c.bufId = buf; c.availF = 640;
    Block p; p.idx = 116; p.m[0] = m0; p.m[5] = 1.7778f; p.m[8] = 0.0007f; p.m[11] = 1.f; p.m[14] = 0.1f;
    Block n = p; n.idx = 132; n.m[8] = 0.f;
    Block c2 = {}; c2.idx = 492; for (int i = 0; i < 16; i++) c2.m[i] = (i % 5) == 0 ? 1.f : 0.f; c2.m[12] = 0.0002f;
    c.blocks = { p, n, c2 };
    return c;
}

int main() {
    Header h; strcpy(h.tag, "1.1.7 test"); h.startFrame = 14300; h.projOff = 112; h.noAAOff = 128; h.mvOff = 544; h.cutFov = 8; h.jitSX = 1; h.jitSY = -1; h.flags = FLAG_SPARSE;
    Writer w; w.Begin(h);
    w.AddCall(MakeCall(14311, 0xA1, 1.0f)); w.AddCall(MakeCall(14312, 0xB2, 1.6406f)); w.AddCall(MakeCall(14313, 0xA1, 1.0f));
    w.AddMark(14312, 7);
    Expect e; e.frame = 14312; e.ev = EV_ALT_VIEW; e.a0 = 1; e.a1 = -5; w.AddExpect(e);

    Trace t; std::string err;
    CHECK(Parse(w.Bytes().data(), w.Bytes().size(), t, &err));
    CHECK(t.h.projOff == 112 && t.h.noAAOff == 128 && t.h.mvOff == 544 && t.h.jitSY == -1 && t.h.startFrame == 14300 && !strcmp(t.h.tag, "1.1.7 test") && (t.h.flags & FLAG_SPARSE));
    CHECK(t.items.size() == 5);
    CHECK(t.items[0].type == REC_CALL && t.items[0].call.frame == 14311 && t.items[0].call.bufId == 0xA1 && t.items[0].call.blocks.size() == 3);
    CHECK(t.items[1].call.blocks[0].m[0] == 1.6406f && t.items[1].call.effW == 1400 && t.items[1].call.effH == 788);
    CHECK(t.items[3].type == REC_MARK && t.items[3].markFrame == 14312 && t.items[3].markCode == 7);
    CHECK(t.items[4].type == REC_EXPECT && t.items[4].ex.ev == EV_ALT_VIEW && t.items[4].ex.a1 == -5);

    std::vector<float> buf; Materialize(t.items[0].call, buf);   // sparse -> full buffer, uncovered floats are zero
    CHECK(buf.size() == 640 && buf[116] == 1.0f && buf[121] == 1.7778f && buf[132] == 1.0f && buf[500] == 0.f && buf[504] == 0.0002f && buf[0] == 0.f && buf[639] == 0.f);

    // a block that would run past availF is clipped, not written out of range
    { Call c = MakeCall(1, 1, 1.f); c.availF = 120; Materialize(c, buf); CHECK(buf.size() == 120 && buf[116] == 1.0f); }

    // unknown record type is skipped
    { std::vector<uint8_t> d = w.Bytes(); d.push_back(0x7F); d.push_back(3); d.push_back(0); d.push_back(0); d.push_back(0); d.push_back(1); d.push_back(2); d.push_back(3);
      Trace t2; CHECK(Parse(d.data(), d.size(), t2, &err) && t2.items.size() == 5); }

    // header / record damage is rejected, never read out of range
    { std::vector<uint8_t> d = w.Bytes(); d[0] = 'X'; Trace t2; CHECK(!Parse(d.data(), d.size(), t2, &err)); }
    { std::vector<uint8_t> d = w.Bytes(); d[4] = 9; Trace t2; CHECK(!Parse(d.data(), d.size(), t2, &err)); }
    { Trace t2; CHECK(!Parse(w.Bytes().data(), 40, t2, &err)); }
    for (size_t cut = kHeaderBytes + 1; cut < w.Bytes().size(); cut += 7) {   // every truncation point inside a record must fail cleanly (or land exactly on a record boundary)
        Trace t2; bool ok = Parse(w.Bytes().data(), cut, t2, &err); if (ok) CHECK(t2.items.size() <= 5);
    }
    srand(1234);   // fuzz: random byte flips must never crash (run under ASan/UBSan in CI)
    for (int it = 0; it < 3000; it++) {
        std::vector<uint8_t> d = w.Bytes(); int nf = 1 + rand() % 6; for (int k = 0; k < nf; k++) d[rand() % d.size()] = (uint8_t)rand();
        Trace t2; Parse(d.data(), d.size(), t2, nullptr);
    }
    // file round trip + text dump runs
    CHECK(w.Save("/tmp/camtrace_test.snct")); Trace t3; CHECK(Load("/tmp/camtrace_test.snct", t3, &err) && t3.items.size() == 5);
    FILE* nul = fopen("/dev/null", "w"); if (nul) { DumpText(nul, t3); fclose(nul); }

    if (g_fail) { printf("camtrace tests FAILED (%d)\n", g_fail); return 1; }
    printf("camtrace tests OK (call record = %zu bytes with 3 blocks)\n", (size_t)(5 + 16 + 3 * 66));
    return 0;
}
