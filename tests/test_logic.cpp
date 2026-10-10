// tests/test_logic.cpp - unit tests for src/logic.h (pure helpers). Build + run: sh tests/run_tests.sh
#include "../src/logic.h"
#include <cstdio>
#include <cstdlib>
static int g_fail = 0, g_n = 0;
#define CHECK(c) do { g_n++; if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
int main(int argc, char** argv) {
    // ---- presets
    CHECK(SnReflexModeParse("off") == 0); CHECK(SnReflexModeParse("OFF\r\n") == 0); CHECK(SnReflexModeParse("0") == 0); CHECK(SnReflexModeParse("lowlatency") == 1); CHECK(SnReflexModeParse("Boost") == 2); CHECK(SnReflexModeParse("2") == 2);
    CHECK(SnReflexModeParse("") == 1); CHECK(SnReflexModeParse("nonsense") == 1); for (int i = 0; i < 3; i++) CHECK(SnReflexModeParse(SnReflexModeName(i)) == i);
    CHECK(SnPresetParse("Quality") == SN_PRESET_QUALITY); CHECK(SnPresetParse("balanced\r\n") == SN_PRESET_BALANCED); CHECK(SnPresetParse("PERFORMANCE") == SN_PRESET_PERFORMANCE); CHECK(SnPresetParse("custom") == SN_PRESET_CUSTOM); CHECK(SnPresetParse("nonsense") == SN_PRESET_CUSTOM); CHECK(SnPresetParse("") == SN_PRESET_CUSTOM);
    { SnPreset p; CHECK(SnPresetGet(SN_PRESET_BALANCED, &p) && p.fgMinFps == 24 && p.fgDelay == 30 && p.camStale == 30 && p.mult == 2);
      CHECK(SnPresetGet(SN_PRESET_QUALITY, &p) && p.fgMinFps == 30 && p.mult == 2); CHECK(SnPresetGet(SN_PRESET_PERFORMANCE, &p) && p.fgMinFps == 0 && p.mult == 4); CHECK(!SnPresetGet(SN_PRESET_CUSTOM, &p)); }
    for (int i = 0; i < SN_PRESET_COUNT; i++) CHECK(SnPresetParse(SnPresetName(i)) == i);
    // ---- output fps cap
    CHECK(SnAutoOutCap(165) == 162); CHECK(SnAutoOutCap(60) == 57); CHECK(SnAutoOutCap(144) == 141); CHECK(SnAutoOutCap(0) == 0); CHECK(SnAutoOutCap(1) == 0);
    CHECK(SnResolveOutCap(-1, 144) == 141); CHECK(SnResolveOutCap(0, 144) == 0); CHECK(SnResolveOutCap(90, 144) == 90);
    CHECK(SnBaseLimit(0, 0, 2, true) == 0); CHECK(SnBaseLimit(141, 0, 2, true) == 70); CHECK(SnBaseLimit(141, 0, 4, true) == 35); CHECK(SnBaseLimit(141, 0, 2, false) == 141);
    CHECK(SnBaseLimit(141, 50, 2, true) == 50); CHECK(SnBaseLimit(141, 100, 2, true) == 70); CHECK(SnBaseLimit(0, 60, 2, true) == 60); CHECK(SnBaseLimit(20, 0, 4, true) == 10);
    // ---- hudless decision
    { double a[] = { 0.97, 0.02, 0.01, 0.0, 0.03 }; CHECK(SnHudDecideFrame(a, 5) == 2);                       // one scene pass, then UI -> copy before draw 2 (the old default)
      double b[] = { 0.99, 0.95, 0.04, 0.02, 0.0 }; CHECK(SnHudDecideFrame(b, 5) == 3);                       // two scene passes
      double c[] = { 0.01, 0.0, 0.02, 0.0 }; CHECK(SnHudDecideFrame(c, 4) == 0);                              // static frame, nothing changed
      double d[] = { 0.9, 0.9, 0.9, 0.9 }; CHECK(SnHudDecideFrame(d, 4) == 0);                                // never leaves the full-screen run
      double e[] = { 0.05, 0.8, 0.03, 0.02 }; CHECK(SnHudDecideFrame(e, 4) == 3);                             // small draw first (letter-box bar), then the scene pass
      double f[] = { 0.62, 0.01 }; CHECK(SnHudDecideFrame(f, 2) == 2); CHECK(SnHudDecideFrame(f, 0) == 0); }
    { std::vector<int> v = { 2, 2, 2, 3, 2, 2 }; int ag = 0; CHECK(SnHudVote(v, 5, 0.7, &ag) == 2 && ag == 5); CHECK(SnHudVote(v, 7, 0.7) == 0); std::vector<int> w = { 2, 3, 2, 3, 4, 2 }; CHECK(SnHudVote(w, 5, 0.7) == 0); }
    { uint8_t a[4] = { 100, 100, 100, 255 }, b[4] = { 102, 99, 100, 0 }, c[4] = { 120, 100, 100, 255 }; CHECK(!SnPixDiffer(SN_PIX_U8x4, a, b)); CHECK(SnPixDiffer(SN_PIX_U8x4, a, c)); }
    { uint32_t x = (500u) | (500u << 10) | (500u << 20), y = (505u) | (500u << 10) | (500u << 20), z = (700u) | (500u << 10) | (500u << 20); CHECK(!SnPixDiffer(SN_PIX_R10G10B10A2, (uint8_t*)&x, (uint8_t*)&y)); CHECK(SnPixDiffer(SN_PIX_R10G10B10A2, (uint8_t*)&x, (uint8_t*)&z)); }
    { uint16_t one = 0x3C00, two = 0x4000, h[4] = { one, one, one, one }, k[4] = { one, two, one, one }, m[4] = { one, one, one, 0 }; CHECK(SnHalf(one) == 1.0f && SnHalf(two) == 2.0f && SnHalf(0xC000) == -2.0f && SnHalf(0x3800) == 0.5f);
      CHECK(SnPixDiffer(SN_PIX_F16x4, (uint8_t*)h, (uint8_t*)k)); CHECK(!SnPixDiffer(SN_PIX_F16x4, (uint8_t*)h, (uint8_t*)m)); }
    { uint8_t A[16] = { 0, 0, 0, 0, 10, 10, 10, 0, 20, 20, 20, 0, 30, 30, 30, 0 }, B[16] = { 0, 0, 0, 0, 10, 10, 10, 0, 90, 90, 90, 0, 90, 90, 90, 0 }; CHECK(SnChangedFraction(SN_PIX_U8x4, A, B, 4, 4) == 0.5); CHECK(SnChangedFraction(SN_PIX_U8x4, A, B, 0, 4) == 0.0); }
    // ---- anti-cheat names
    CHECK(SnAcName("easyanticheat") && !strcmp(SnAcName("easyanticheat"), "Easy Anti-Cheat")); CHECK(SnAcName("easyanticheat_eos_setup.exe")); CHECK(SnAcName("start_protected_game.exe")); CHECK(SnAcName("easyanticheat_x64.dll"));
    CHECK(SnAcName("battleye") && SnAcName("beservice_x64.exe") && SnAcName("beclient_x64.dll") && !strcmp(SnAcName("bedaisy.sys"), "BattlEye"));
    CHECK(SnAcName("gameguard") && SnAcName("npggnt.des") && SnAcName("npgg64.des")); CHECK(SnAcName("xigncode") && SnAcName("x3.xem")); CHECK(SnAcName("vgc.exe")); CHECK(SnAcName("anticheatexpert")); CHECK(SnAcName("pbcl.dll"));
    CHECK(!SnAcName("scarletnexus-win64-shipping.exe")); CHECK(!SnAcName("binaries")); CHECK(!SnAcName("engine")); CHECK(!SnAcName("winmm.dll")); CHECK(!SnAcName("beautiful.exe")); CHECK(!SnAcName("eossdk-win64-shipping.dll")); CHECK(!SnAcName(""));
    // ---- UE version scan
    { const char* u4 = "xxxx++UE4+Release-4.26yy"; const char* u5 = "zz++UE5+Release-5.3"; CHECK(SnScanUeVersion((const uint8_t*)u4, strlen(u4)) == 4); CHECK(SnScanUeVersion((const uint8_t*)u5, strlen(u5)) == 5);
      const uint8_t w4[] = { 'a', 0, '+', 0, '+', 0, 'U', 0, 'E', 0, '4', 0, '+', 0, 'R', 0 }, w5[] = { '+', 0, '+', 0, 'U', 0, 'E', 0, '5', 0, '+', 0 }; CHECK(SnScanUeVersion(w4, sizeof w4) == 4); CHECK(SnScanUeVersion(w5, sizeof w5) == 5);
      const char* none = "just some text ++ and + UE4"; CHECK(SnScanUeVersion((const uint8_t*)none, strlen(none)) == 0); const char* both = "++UE4+Release-4.27 ... ++UE5+Release-5.0"; CHECK(SnScanUeVersion((const uint8_t*)both, strlen(both)) == 5); CHECK(SnScanUeVersion(nullptr, 0) == 0); }
    // ---- crc / zip
    { const char* s = "123456789"; CHECK(SnCrc32((const uint8_t*)s, 9) == 0xCBF43926u); CHECK(SnCrc32((const uint8_t*)"", 0) == 0); uint32_t c1 = SnCrc32((const uint8_t*)s, 4); CHECK(SnCrc32((const uint8_t*)s + 4, 5, c1) == 0xCBF43926u); }
    { SnZip z; z.SetTime(2026, 10, 10, 12, 30, 40); z.Add("a.txt", "hello world\n"); z.Add("dir/b.log", std::string(100000, 'x')); z.Add("empty.txt", ""); std::string out = z.Finish();
      CHECK(out.size() > 100000); CHECK(out.compare(0, 4, "PK\x03\x04", 4) == 0);
      if (argc > 1) { FILE* f = fopen(argv[1], "wb"); if (f) { fwrite(out.data(), 1, out.size(), f); fclose(f); } } }   // run_tests.sh verifies it with unzip -t
    // ---- redaction / tail
    CHECK(SnRedactUser("C:\\Users\\Alice\\Games\\x", "alice") == "C:\\Users\\<user>\\Games\\x"); CHECK(SnRedactUser("/mnt/Users/ALICE/a and Alice", "Alice") == "/mnt/Users/<user>/a and Alice"); CHECK(SnRedactUser("nothing here", "alice") == "nothing here"); CHECK(SnRedactUser("abc", "") == "abc");
    CHECK(SnRedactUser("C:\\Users\\Alicia\\x", "Alice") == "C:\\Users\\Alicia\\x");
    { std::string s; for (int i = 0; i < 100; i++) s += "line " + std::to_string(i) + "\n"; std::string t = SnTail(s, 100); CHECK(t.size() < 160); CHECK(t.find("line 99") != std::string::npos); CHECK(t.find("line 0\n") == std::string::npos); CHECK(SnTail("short", 100) == "short"); }
    // ---- config rewrite
    { std::map<std::string, std::string> ch = { { "fg", "0" }, { "newkey", "7" } };
      std::string in = "# comment fg=1\r\nfg=1\r\nmult=2\r\n  mult2 = 5\r\n"; std::string out = SnCfgApplyChanges(in, ch);
      CHECK(out.find("# comment fg=1\r\n") == 0); CHECK(out.find("\r\nfg=0\r\n") != std::string::npos); CHECK(out.find("mult=2\r\n") != std::string::npos); CHECK(out.find("newkey=7\r\n") != std::string::npos); CHECK(out.find("fg=1\r\nmult") == std::string::npos);
      std::string in2 = "a=1\nb=2"; std::string o2 = SnCfgApplyChanges(in2, { { "b", "9" } }); CHECK(o2 == "a=1\nb=9\n"); CHECK(SnCfgApplyChanges("", { { "x", "1" } }).find("x=1\n") != std::string::npos); }
    printf("%d checks, %d failed\n", g_n, g_fail); return g_fail ? 1 : 0;
}
