#!/bin/sh
# Generates winmm_stubs.inc + winmm.def from the REAL export list of mingw's libwinmm.a
set -e
CC=${CC:-x86_64-w64-mingw32}
LIB=$(${CC}-g++-posix -print-file-name=libwinmm.a 2>/dev/null || true)
[ -f "$LIB" ] || LIB=/usr/${CC}/lib/libwinmm.a
[ -f "$LIB" ] || { echo "libwinmm.a not found"; exit 1; }
${CC}-nm -g --defined-only "$LIB" 2>/dev/null | awk '$2=="T"{print $3}' | grep -v '^__imp_' | sort -u > winmm_exports.txt
N=$(wc -l < winmm_exports.txt)
echo "winmm exports: $N"
{
  echo "#define WINMM_EXPORT_COUNT $N"
  echo "static const char* const g_fwdNames[WINMM_EXPORT_COUNT] = {"
  awk '{printf "\"%s\",\n",$1}' winmm_exports.txt
  echo "};"
  printf '%s\n' '__asm__(".text\n"'
  awk '{printf "\".globl %s\\n%s:\\n jmp *g_fwd+%d(%%rip)\\n\"\n",$1,$1,(NR-1)*8}' winmm_exports.txt
  echo ');'
} > winmm_stubs.inc
{ echo "LIBRARY winmm"; echo "EXPORTS"; cat winmm_exports.txt; } > winmm.def
