@echo off
rem 后台启动（无控制台窗口）。日志写到 exe 同目录的 fix_explorer_refresh.log
"%~dp0fix_explorer_refresh.exe" --detach
pause
