@echo off
setlocal

set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Debug

set ROOT=%~dp0
set SRC=%ROOT%build\vs2013\bin\win32\%CONFIG%

if not exist "%SRC%\skynet.exe" (
    echo [copybin] ERROR: "%SRC%\skynet.exe" not found.
    echo [copybin] Build the VS2013 solution first ^(Configuration=%CONFIG%^).
    exit /b 1
)

echo [copybin] Syncing %CONFIG% build artifacts to repo root...

REM exe + runtime DLLs (must sit next to skynet.exe)
copy /Y "%SRC%\skynet.exe"  "%ROOT%skynet.exe" >nul
copy /Y "%SRC%\luaexe.exe"  "%ROOT%lua.exe"    >nul
copy /Y "%SRC%\lua.dll"     "%ROOT%lua.dll"    >nul
copy /Y "%SRC%\posix.dll"   "%ROOT%posix.dll"  >nul

REM cservice (C services loaded by skynet)
if not exist "%ROOT%cservice" mkdir "%ROOT%cservice"
del /Q "%ROOT%cservice\*.so" 2>nul
copy /Y "%SRC%\cservice\*.so" "%ROOT%cservice\" >nul

REM luaclib (Lua C extensions required by Lua services)
if not exist "%ROOT%luaclib" mkdir "%ROOT%luaclib"
del /Q "%ROOT%luaclib\*.so" 2>nul
copy /Y "%SRC%\luaclib\*.so" "%ROOT%luaclib\" >nul

echo [copybin] Done. From repo root, run:
echo   skynet.exe examples\config
echo   lua.exe    examples\client.lua

endlocal
