// detect.inc (v27) - things the injector finds out about the game BEFORE it hooks anything:
//   * which Unreal Engine major version the exe is (4 / 5) - only used to write the right default config the first time (no game database, nothing to maintain)
//   * whether an anti-cheat is present (files next to the exe / loaded modules). Then the injector switches itself OFF (anticheat=1, default).
// Pure name lists + the scan loop live in logic.h (SnAcName, SnScanUeVersion); this file only walks the file system / the module list.
#pragma once
#include <tlhelp32.h>

// 4 = UE4, 5 = UE5, 0 = not found. Reads the .rdata / .data of the game's own exe image once (only when no config exists yet).
static int DetectUeVersion() {
    HMODULE m = GetModuleHandleW(nullptr); if (!m) return 0;
    const uint8_t* base = (const uint8_t*)m; auto dos = (const IMAGE_DOS_HEADER*)base; if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew); if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt); int best = 0; DWORD t0 = GetTickCount();
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections && best < 5; i++) {
        char nm[9] = {}; memcpy(nm, sec[i].Name, 8); if (strcmp(nm, ".rdata") && strcmp(nm, ".data")) continue;
        const uint8_t* p = base + sec[i].VirtualAddress; size_t left = sec[i].Misc.VirtualSize; if (left > (size_t)(512u << 20)) left = (size_t)(512u << 20);
        while (left && best < 5) {   // walk committed, readable regions only (never touch a page that could fault)
            MEMORY_BASIC_INFORMATION mbi; if (!VirtualQuery(p, &mbi, sizeof mbi)) break;
            size_t len = std::min<size_t>(left, (size_t)(((const uint8_t*)mbi.BaseAddress + mbi.RegionSize) - p)); if (!len) break;
            if (mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
                const size_t blk = 4u << 20; for (size_t o = 0; o < len && best < 5; o += blk - 16) { size_t n = std::min(blk, len - o); best = std::max(best, SnScanUeVersion(p + o, n)); if (n < blk) break; }
            }
            p += len; left -= len;
        }
    }
    Log("engine scan: %s (%u ms)", best == 5 ? "Unreal Engine 5" : best == 4 ? "Unreal Engine 4" : "no Unreal version string found", (unsigned)(GetTickCount() - t0));
    return best;
}

// ---- anti-cheat guard
static std::atomic<bool> g_acTripped{ false };
static std::string AcNarrow(const wchar_t* w) { char b[1024]; int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr); return n > 0 ? std::string(b, (size_t)n - 1) : std::string(); }
static std::string AcLowerNarrow(const wchar_t* w) { char b[512]; int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr); return n > 0 ? SnLower(std::string(b, (size_t)n - 1)) : std::string(); }
// exe folder + 3 folders up (UE layout: <Root>\<Game>\Binaries\Win64\game.exe, the anti-cheat lives next to <Game> or in Binaries\Win64)
static bool AcScanFiles(std::string& label, std::string& where) {
    std::wstring d = g_dir;
    for (int lvl = 0; lvl < 4 && !d.empty(); lvl++) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            int n = 0;
            do { if (++n > 4000) break; if (fd.cFileName[0] == L'.' ) continue; std::string nm = AcLowerNarrow(fd.cFileName); const char* p = SnAcName(nm);
                 if (p) { label = p; where = "file/folder '" + nm + "' in " + AcNarrow(d.c_str()); FindClose(h); return true; } } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        size_t cut = d.find_last_of(L'\\'); if (cut == std::wstring::npos) break; d = d.substr(0, cut);
    }
    return false;
}
static bool AcScanModules(std::string& label, std::string& where) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId()); if (s == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me = {}; me.dwSize = sizeof me; bool hit = false;
    if (Module32FirstW(s, &me)) do { std::string nm = AcLowerNarrow(me.szModule); const char* p = SnAcName(nm); if (p) { label = p; where = "loaded module '" + nm + "'"; hit = true; break; } } while (Module32NextW(s, &me));
    CloseHandle(s); return hit;
}
static DWORD WINAPI AcNoticeThread(LPVOID p) {
    std::string* s = (std::string*)p; wchar_t w[1400]; MultiByteToWideChar(CP_UTF8, 0, s->c_str(), -1, w, 1400); delete s;
    MessageBoxW(nullptr, w, L"SN_DLSSG - switched off (anti-cheat)", MB_OK | MB_ICONINFORMATION | MB_TOPMOST); return 0;
}
static void AcNotice(const std::string& label, const std::string& where) {
    Log("ANTI-CHEAT GUARD: %s detected (%s) -> frame generation injector switched OFF (nothing hooked). Set anticheat=0 in SN_DLSSG_cfg.txt only for an offline / anti-cheat-free launch and at your own risk.", label.c_str(), where.c_str());
    if (!g_cfg.msgBox) return;
    auto* m = new std::string("SN_DLSSG found the anti-cheat \"" + label + "\" (" + where + ").\n\nTo protect your account the injector has switched itself OFF: nothing is hooked and the game runs unmodified.\n\n"
        "Playing without the anti-cheat (offline mode / launched without it)? Then set  anticheat=0  in SN_DLSSG_cfg.txt - at your own risk. Never use this in online matches.\n\n(Remove winmm.dll to get rid of this message. msgbox=0 hides it.)");
    HANDLE h = CreateThread(nullptr, 0, AcNoticeThread, m, 0, nullptr); if (h) CloseHandle(h); else delete m;
}
// later check from the Present hook (anti-cheat modules often load after start-up): switch frame generation off again
static void AcLateCheck(int fr) {
    if (!g_cfg.antiCheat || g_acTripped.load() || !(fr == 120 || fr == 1800 || fr == 7200)) return;
    std::string l, w; if (AcScanModules(l, w)) { g_acTripped = true; g_cfg.fg = 0; AcNotice(l, w); }   // fg=0 -> the normal 'DLSS-G off' path runs; wantOn is also blocked by g_acTripped
}
