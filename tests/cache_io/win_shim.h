// minimal Win32 file API on top of POSIX - just enough for the storage layer of psocache.inc
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
typedef void* HANDLE; typedef uint32_t DWORD; typedef long long LONGLONG; typedef int BOOL; typedef const wchar_t* LPCWSTR;
typedef union { struct { uint32_t LowPart; int32_t HighPart; }; LONGLONG QuadPart; } LARGE_INTEGER;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_APPEND_DATA 4u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define OPEN_EXISTING 3
#define CREATE_ALWAYS 2
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_BEGIN 0
#define MOVEFILE_REPLACE_EXISTING 1
#define MOVEFILE_WRITE_THROUGH 8
static std::string Nar(const std::wstring& w) { std::string s; for (wchar_t c : w) s.push_back((char)c); return s; }
static HANDLE CreateFileW(LPCWSTR p, DWORD acc, DWORD, void*, DWORD disp, DWORD, void*) {
    int fl = 0; std::string n = Nar(p);
    if (acc & FILE_APPEND_DATA) fl = O_WRONLY | O_APPEND;
    else if (acc & GENERIC_WRITE) fl = O_WRONLY | (disp == CREATE_ALWAYS ? (O_CREAT | O_TRUNC) : 0);
    else fl = O_RDONLY;
    int fd = open(n.c_str(), fl, 0644); if (fd < 0) return INVALID_HANDLE_VALUE; return (HANDLE)(intptr_t)(fd + 1);
}
static int FD(HANDLE h) { return (int)(intptr_t)h - 1; }
static BOOL ReadFile(HANDLE h, void* b, DWORD n, DWORD* r, void*) { ssize_t k = read(FD(h), b, n); if (k < 0) { *r = 0; return 0; } *r = (DWORD)k; return 1; }
static BOOL WriteFile(HANDLE h, const void* b, DWORD n, DWORD* w, void*) { ssize_t k = write(FD(h), b, n); if (k < 0) { *w = 0; return 0; } *w = (DWORD)k; return 1; }
static BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* s) { struct stat st; if (fstat(FD(h), &st)) return 0; s->QuadPart = st.st_size; return 1; }
static BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER p, LARGE_INTEGER*, DWORD) { return lseek(FD(h), p.QuadPart, SEEK_SET) >= 0; }
static BOOL SetEndOfFile(HANDLE h) { off_t o = lseek(FD(h), 0, SEEK_CUR); return ftruncate(FD(h), o) == 0; }
static BOOL FlushFileBuffers(HANDLE h) { return fsync(FD(h)) == 0; }
static BOOL CloseHandle(HANDLE h) { return close(FD(h)) == 0; }
static BOOL MoveFileExW(LPCWSTR a, LPCWSTR b, DWORD) { return rename(Nar(a).c_str(), Nar(b).c_str()) == 0; }
static BOOL DeleteFileW(LPCWSTR a) { return unlink(Nar(a).c_str()) == 0; }
static DWORD GetTickCount() { return 0; }
static void Sleep(DWORD) {}
static DWORD GetLastError() { return 5; }
static int g_logQuiet = 1;
static void Log(const char* f, ...) { if (g_logQuiet) return; va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); }
static void LogW(const wchar_t*) {}
struct Cfg { int psoCache = 1, psoCacheMB = 384, cacheCompress = 1; }; static Cfg g_cfg;
static BOOL CreateDirectoryW(LPCWSTR a, void*) { return mkdir(Nar(a).c_str(), 0755) == 0; }
