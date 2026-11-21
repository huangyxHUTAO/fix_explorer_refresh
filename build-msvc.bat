@echo off
rem Visual Studio command prompt (cl.exe)
cl /nologo /O2 /W3 fix_explorer_refresh.c /link ole32.lib oleaut32.lib uuid.lib shell32.lib shlwapi.lib user32.lib
if errorlevel 1 (
  echo build FAILED
  exit /b 1
)
del fix_explorer_refresh.obj 2>/dev/null
echo OK: fix_explorer_refresh.exe
