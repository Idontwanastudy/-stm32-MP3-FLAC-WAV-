@echo off
rem ============================================
rem  fix_fpu.bat
rem  Run this AFTER a Clean in CubeIDE.
rem  It replaces -mfloat-abi=soft with
rem  -mfloat-abi=hard in all Debug makefiles.
rem ============================================
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -Command "Get-ChildItem -Path 'Debug' -Recurse -Include '*.mk','makefile' -File | ForEach-Object { $c = [System.IO.File]::ReadAllText($_.FullName); $c = $c.Replace('-mfloat-abi=soft','-mfloat-abi=hard'); [System.IO.File]::WriteAllText($_.FullName, $c) }"
echo.
echo FPU fix done. Go back to CubeIDE and press Build.
echo.
pause
