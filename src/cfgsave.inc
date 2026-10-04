// cfgsave.inc  (v25) - write settings that were changed in the in-game menu back into SN_DLSSG_cfg.txt
// The file is rewritten line by line: comments, unknown keys and the order of everything else stay exactly as the user left them.
// Keys that are not in the file yet are appended at the end. The new file is written next to the old one and swapped in (never a half-written config).

static std::mutex g_cfgSaveMx;
static std::map<std::string, std::string> g_cfgPending;
static std::atomic<DWORD> g_cfgPendTick{ 0 };
static std::atomic<bool> g_cfgFlushBusy{ false };
static std::atomic<int> g_cfgSaveState{ 0 };     // 0 = nothing saved yet, 1 = last save ok, -1 = last save failed (read-only folder?)
static std::atomic<DWORD> g_cfgSavedTick{ 0 };

static void CfgSet(const char* key, const std::string& val) {
    if (!g_cfg.autoSave) return;
    std::lock_guard<std::mutex> lk(g_cfgSaveMx);
    g_cfgPending[key] = val; g_cfgPendTick.store(GetTickCount());
}
static void CfgSetInt(const char* key, int v) { CfgSet(key, std::to_string(v)); }
static void CfgSaveSoon() { if (g_cfgPendTick.load()) g_cfgPendTick.store(GetTickCount() - 5000); }   // flush at the next frame

// pure text transformation (unit-tested): returns the new file content
static std::string CfgApplyChanges(const std::string& in, const std::map<std::string, std::string>& changes) {
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

static bool CfgWriteNow() {
    std::map<std::string, std::string> pend;
    { std::lock_guard<std::mutex> lk(g_cfgSaveMx); pend.swap(g_cfgPending); g_cfgPendTick.store(0); }
    if (pend.empty()) return true;
    std::wstring path = g_dir + L"\\SN_DLSSG_cfg.txt", tmp = path + L".tmp";
    std::string content;
    if (FILE* f = _wfopen(path.c_str(), L"rb")) { char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) content.append(b, n); fclose(f); }
    std::string out = CfgApplyChanges(content, pend);
    bool ok = false;
    if (FILE* f = _wfopen(tmp.c_str(), L"wb")) {
        ok = fwrite(out.data(), 1, out.size(), f) == out.size(); ok = (fclose(f) == 0) && ok;
        if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
        if (!ok) DeleteFileW(tmp.c_str());
    }
    g_cfgSaveState.store(ok ? 1 : -1); if (ok) g_cfgSavedTick.store(GetTickCount());
    Log("config %s (%zu setting(s) written to SN_DLSSG_cfg.txt)", ok ? "saved" : "SAVE FAILED (folder read-only?)", pend.size());
    return ok;
}
static DWORD WINAPI CfgFlushThread(LPVOID) { try { CfgWriteNow(); } catch (...) {} g_cfgFlushBusy.store(false); return 0; }
// called once per frame by the overlay: settle for ~1.2 s after the last change, then write on a short-lived thread (no disk I/O on the render thread)
static void CfgPoll() {
    DWORD t = g_cfgPendTick.load(); if (!t) return;
    if (GetTickCount() - t < 1200) return;
    if (g_cfgFlushBusy.exchange(true)) return;
    HANDLE h = CreateThread(nullptr, 0, CfgFlushThread, nullptr, 0, nullptr);
    if (h) CloseHandle(h); else g_cfgFlushBusy.store(false);
}
