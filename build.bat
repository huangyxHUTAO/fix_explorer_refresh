@echo off
rem MinGW-w64 / GCC
gcc -O2 -municode -s -o fix_explorer_refresh.exe fix_explorer_refresh.c -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -luser32
if errorlevel 1 (
  echo build FAILED
  exit /b 1
)
echo OK: fix_explorer_refresh.exe
