// crash.inc (v27) - crash mini-dumps + one-click support bundle
//
//   * A crash that happens INSIDE this injector / Streamline / NGX / the NVIDIA driver (or an unfixable fast-fail / heap-corruption crash anywhere) writes
//       SN_DLSSG_crash_<date>_<pid>.dmp   (mini-dump, opens in Visual Studio / WinDbg)   +   SN_DLSSG_crash_<date>_<pid>.txt  (what / where / last log lines)
//     next to the game exe. Crashes that nobody handles also get a dump (unhandled-exception filter, chained in front of the game's own one).
//     At most 3 dumps per run, never more than one per 10 s. crashdump=0 switches all of it off.
//   * The menu entry "Create support zip" collects config, log, sl.log, debug report, view-buffer dump and the newest crash files into ONE zip
//     (SN_DLSSG_support_<date>.zip). Text files have the Windows user name replaced by <user>.
#pragma once
#include <dbghelp.h>

static std::atomic<int> g_dumpCount{ 0 }; static std::atomic<DWORD> g_lastDumpTick{ 0 }; static std::atomic<bool> g_inCrash{ false };
typedef BOOL(WINAPI* PFN_MDWD)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
static PFN_MDWD g_pMDWD = nullptr; static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;

static std::wstring StampW() { SYSTEMTIME t; GetLocalTime(&t); wchar_t b[48]; swprintf(b, 48, L"%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); return b; }
static std::wstring TempDirW() { wchar_t t[MAX_PATH]; DWORD n = GetTempPathW(MAX_PATH, t); return n ? std::wstring(t, n) : std::wstring(L".\\"); }
// opens <g_dir>\name for writing (falls back to the temp folder); returns the handle and the path that worked
static HANDLE CreateOutFile(const std::wstring& name, std::wstring& pathOut) {
    std::wstring p = g_dir + L"\\" + name; HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { p = TempDirW() + name; h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); }
    pathOut = p; return h;
}
static bool ModuleOfAddr(const void* a, char* base, size_t n, ULONG_PTR* off, HMODULE* mod) {
    HMODULE m = nullptr; base[0] = 0; if (mod) *mod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) || !m) return false;
    char full[MAX_PATH] = {}; GetModuleFileNameA(m, full, MAX_PATH); const char* b = strrchr(full, '\\'); b = b ? b + 1 : full; snprintf(base, n, "%s", b);
    if (off) *off = (ULONG_PTR)((const BYTE*)a - (const BYTE*)m); if (mod) *mod = m; return true;
}
static bool CrashModuleIsOurs(const void* a) {
    char b[MAX_PATH]; HMODULE m = nullptr; if (!ModuleOfAddr(a, b, sizeof b, nullptr, &m)) return false;
    if (m == g_hSelf) return true; std::string l = SnLower(b);
    return l.compare(0, 3, "sl.") == 0 || l.compare(0, 5, "nvngx") == 0 || l.compare(0, 8, "nvwgf2um") == 0 || l.compare(0, 5, "nvapi") == 0 || l.compare(0, 6, "nvcuda") == 0;
}
static bool CrashFaultIsOurs(PEXCEPTION_POINTERS ep) {
    if (CrashModuleIsOurs(ep->ExceptionRecord->ExceptionAddress)) return true;
    void* st[14]; USHORT n = CaptureStackBackTrace(0, 14, st, nullptr); for (USHORT i = 0; i < n; i++) if (CrashModuleIsOurs(st[i])) return true;
    return false;
}
static void CrashWriteSummary(const std::wstring& txtName, PEXCEPTION_POINTERS ep, const char* why, const std::wstring& dumpPath, bool dumpOk) {
    std::wstring tp; HANDLE h = CreateOutFile(txtName, tp); if (h == INVALID_HANDLE_VALUE) return; CloseHandle(h);
    FILE* f = _wfopen(tp.c_str(), L"w"); if (!f) return;
    fprintf(f, "SN_DLSSG " SN_VERSION " crash summary\nreason   : %s\n", why);
    if (ep && ep->ExceptionRecord) { char mn[MAX_PATH]; ULONG_PTR off = 0; void* a = ep->ExceptionRecord->ExceptionAddress; ModuleOfAddr(a, mn, sizeof mn, &off, nullptr);
        fprintf(f, "exception: 0x%08X at %p  (%s + 0x%llX)  thread %u\n", (unsigned)ep->ExceptionRecord->ExceptionCode, a, mn[0] ? mn : "?", (unsigned long long)off, (unsigned)GetCurrentThreadId());
        void* st[24]; USHORT n = CaptureStackBackTrace(0, 24, st, nullptr); fprintf(f, "stack    :\n");
        for (USHORT i = 0; i < n; i++) { char sm[MAX_PATH]; ULONG_PTR so = 0; ModuleOfAddr(st[i], sm, sizeof sm, &so, nullptr); fprintf(f, "  #%02u %p %s+0x%llX\n", (unsigned)i, st[i], sm[0] ? sm : "?", (unsigned long long)so); } }
    fprintf(f, "dump     : %s\nframe    : %d   fg=%d mult=%d stage=%d preset=%s hudless=%d(N=%d) fpscap=%d\n", dumpOk ? Narrow(dumpPath.c_str()).c_str() : "(could not be written)", g_frame.load(), g_cfg.fg, g_cfg.mult, g_cfg.stage, SnPresetName(g_cfg.preset), g_cfg.hudless, g_cfg.hudlessDraw, g_cfg.fpsCap);
    fprintf(f, "\n---- last log lines ----\n");
    { auto ring = LogRingSnapshot(); size_t from = ring.size() > 120 ? ring.size() - 120 : 0; for (size_t i = from; i < ring.size(); i++) fprintf(f, "%s\n", ring[i].c_str()); }
    fclose(f);
}
static void CrashWrite(PEXCEPTION_POINTERS ep, const char* why) {
    if (!g_cfg.crashDump || !g_pMDWD || g_dumpCount.load() >= 3) return;
    DWORD now = GetTickCount(); if (g_dumpCount.load() > 0 && now - g_lastDumpTick.load() < 10000) return;
    if (g_inCrash.exchange(true)) return;
    g_dumpCount++; g_lastDumpTick = now;
    if (g_log) fflush(g_log);   // (no lock: the crashing thread may be inside Log())
    std::wstring base = L"SN_DLSSG_crash_" + StampW() + L"_" + std::to_wstring(GetCurrentProcessId()), dp; HANDLE h = CreateOutFile(base + L".dmp", dp); bool ok = false;
    if (h != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), ep, FALSE };
        ok = g_pMDWD(GetCurrentProcess(), GetCurrentProcessId(), h, (MINIDUMP_TYPE)(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData), ep ? &mei : nullptr, nullptr, nullptr) != 0;
        CloseHandle(h);
    }
    CrashWriteSummary(base + L".txt", ep, why, dp, ok);
    Log("CRASH: %s -> mini-dump %s (%s)", why, Narrow(dp.c_str()).c_str(), ok ? "written" : "FAILED"); if (g_log) fflush(g_log);
    g_inCrash = false;
}
static LONG CALLBACK CrashVEH(PEXCEPTION_POINTERS ep) {
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    bool fast = (c == 0xC0000409 || c == 0xC0000374);   // fast-fail / heap corruption: no handler can catch these, the process dies right now
    bool code = fast || c == 0xC0000005 || c == 0xC000001D || c == 0xC0000094 || c == 0xC00000FD || c == 0xC000008C || c == 0xC0000096;
    if (!code) return EXCEPTION_CONTINUE_SEARCH;
    if (fast) CrashWrite(ep, "fast-fail / heap corruption (the process is about to die)");
    else if (CrashFaultIsOurs(ep)) CrashWrite(ep, "exception inside SN_DLSSG / Streamline / NGX / NVIDIA driver");
    return EXCEPTION_CONTINUE_SEARCH;
}
static LONG WINAPI CrashFilter(PEXCEPTION_POINTERS ep) {
    CrashWrite(ep, "unhandled exception");
    return (g_prevFilter && g_prevFilter != CrashFilter) ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}
static void CrashChain() { LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(CrashFilter); if (prev && prev != CrashFilter) g_prevFilter = prev; }
static void CrashInit() {
    if (!g_cfg.crashDump) return;
    HMODULE m = LoadLibraryW(L"dbghelp.dll"); if (m) g_pMDWD = (PFN_MDWD)GetProcAddress(m, "MiniDumpWriteDump");   // loaded NOW (not inside a crash: loading a DLL there can deadlock)
    if (!g_pMDWD) { Log("crash dumps: dbghelp.dll / MiniDumpWriteDump not available -> disabled"); return; }
    AddVectoredExceptionHandler(0, CrashVEH); CrashChain();
    Log("crash dumps: armed (crashdump=1) - SN_DLSSG_crash_*.dmp / .txt next to the exe");
}

// ------------------------------------------------------------------ support bundle (zip)
static std::atomic<int> g_bundleState{ 0 };   // 0 idle, 1 running, 2 done, -1 failed
static std::mutex g_bundleMx; static std::string g_bundleMsg; static std::atomic<bool> g_bundleReported{ true };
static bool ReadWholeFile(const std::wstring& p, std::string& out, size_t maxBytes) {
    FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return false; out.clear(); char b[1 << 16]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) { out.append(b, n); if (out.size() > maxBytes) { fclose(f); out.clear(); return false; } }
    fclose(f); return true;
}
static std::string UserNameNarrow() { wchar_t b[256]; DWORD n = GetEnvironmentVariableW(L"USERNAME", b, 256); return (n && n < 256) ? Narrow(b) : std::string(); }
static DWORD WINAPI BundleThread(LPVOID) {
    std::string msg; bool ok = false;
    try {
        try { DiagWriteReport("support bundle (menu)"); } catch (...) {}
        SnZip z; { SYSTEMTIME t; GetLocalTime(&t); z.SetTime(t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); }
        std::string user = UserNameNarrow(), manifest = "SN_DLSSG " SN_VERSION " support bundle\r\n\r\nFiles:\r\n";
        struct TF { const wchar_t* name; size_t cap; bool head; };
        const TF text[] = { { L"SN_DLSSG_cfg.txt", 64u << 10, false }, { L"SN_DLSSG_DEBUG_REPORT.txt", 512u << 10, false }, { L"SN_DLSSG_log.txt", 4u << 20, false }, { L"SN_DLSSG_log_prev.txt", 1u << 20, false },
                            { L"sl.log", 2u << 20, false }, { L"SN_DLSSG_viewbuf.txt", 300u << 10, true } };
        for (auto& t : text) {
            std::string d; if (!ReadWholeFile(g_dir + L"\\" + t.name, d, 128u << 20) || d.empty()) continue;
            d = t.head ? (d.size() > t.cap ? d.substr(0, t.cap) + "\n[... rest cut ...]\n" : d) : SnTail(d, t.cap); d = SnRedactUser(d, user);
            std::string nm = Narrow(t.name); z.Add(nm, d); manifest += "  " + nm + "  (" + std::to_string(d.size()) + " bytes)\r\n";
        }
        // newest crash files (2 x .txt, 2 x .dmp)
        struct CF { std::wstring name; ULONGLONG t; bool dmp; }; std::vector<CF> cf; WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((g_dir + L"\\SN_DLSSG_crash_*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) { do { std::wstring n = fd.cFileName; bool dmp = n.size() > 4 && n.compare(n.size() - 4, 4, L".dmp") == 0; bool txt = n.size() > 4 && n.compare(n.size() - 4, 4, L".txt") == 0; if (dmp || txt) cf.push_back({ n, ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime, dmp }); } while (FindNextFileW(h, &fd)); FindClose(h); }
        std::sort(cf.begin(), cf.end(), [](const CF& a, const CF& b) { return a.t > b.t; }); int nd = 0, nt = 0;
        for (auto& c : cf) { if (c.dmp ? nd >= 2 : nt >= 2) continue; std::string d; if (!ReadWholeFile(g_dir + L"\\" + c.name, d, 24u << 20) || d.empty()) continue; if (!c.dmp) d = SnRedactUser(d, user);
            z.Add(Narrow(c.name.c_str()), d); manifest += "  " + Narrow(c.name.c_str()) + "  (" + std::to_string(d.size()) + " bytes)\r\n"; (c.dmp ? nd : nt)++; }
        manifest += "\r\nPrivacy: in the text files your Windows user name was replaced by <user>. Crash dumps (.dmp) are memory snapshots of the game process and can contain file paths and other text -\r\n"
                    "attach them to a public Issue only if you are comfortable with that (or send them privately).\r\n";
        z.Add("MANIFEST.txt", manifest);
        std::string zip = z.Finish(); std::wstring zp; HANDLE o = CreateOutFile(L"SN_DLSSG_support_" + StampW() + L".zip", zp);
        if (o != INVALID_HANDLE_VALUE) { DWORD w = 0; ok = WriteFile(o, zip.data(), (DWORD)zip.size(), &w, nullptr) && w == zip.size(); CloseHandle(o); }
        std::string nm = Narrow(zp.c_str()); size_t sl = nm.find_last_of('\\'); if (sl != std::string::npos) nm = nm.substr(sl + 1);
        msg = ok ? "Support zip saved next to the game exe: " + nm + "  - attach it to a GitHub Issue." : "Could not write the support zip (folder read-only?).";
        Log("support bundle: %s (%zu bytes)", ok ? Narrow(zp.c_str()).c_str() : "FAILED", zip.size());
    } catch (...) { msg = "Support zip failed (unexpected error)."; ok = false; }
    { std::lock_guard<std::mutex> lk(g_bundleMx); g_bundleMsg = msg; } g_bundleState = ok ? 2 : -1; g_bundleReported = false; return 0;
}
static void BundleStart() { int e = 0; if (!g_bundleState.compare_exchange_strong(e, 1)) { if (g_bundleState.load() == 1) return; g_bundleState = 1; } HANDLE h = CreateThread(nullptr, 0, BundleThread, nullptr, 0, nullptr); if (h) CloseHandle(h); else g_bundleState = 0; }
// called by the overlay every frame: true once when a finished bundle should be announced
static bool BundlePoll(std::string& msg, bool& bad) {
    if (g_bundleReported.load() || g_bundleState.load() == 1) return false;
    { std::lock_guard<std::mutex> lk(g_bundleMx); msg = g_bundleMsg; } bad = g_bundleState.load() < 0; g_bundleReported = true; g_bundleState = 0; return true;
}
