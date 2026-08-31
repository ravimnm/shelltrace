@echo off

echo ==========================
echo ShellTrace Test
echo ==========================

echo.
echo [1] Starting ping...
ping 127.0.0.1 -n 3 > nul

echo.
echo [2] Starting PowerShell...
powershell.exe -NoProfile -Command "Write-Host PowerShell child process"

echo.
echo [3] Starting another command...
cmd.exe /c "echo Nested CMD process"

echo.
echo [4] Intentional failure...
cmd.exe /c "exit /b 7"

echo.
echo [5] Test finished.

exit /b 0