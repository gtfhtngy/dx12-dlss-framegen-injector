@echo off
setlocal EnableExtensions
rem ---------------------------------------------------------------------------------------------
rem  SN_DLSSG installer (v27).  Copies winmm.dll, the config and the six NVIDIA Streamline files
rem  into the game's exe folder. It does NOT download anything: the Streamline DLLs must come from
rem  the official SDK (https://github.com/NVIDIAGameWorks/Streamline, production build, tested 2.14.1).
rem
rem  usage:  install.bat "<game exe folder>" "<folder with the Streamline DLLs>"
rem          install.bat uninstall "<game exe folder>"
rem          (or just double-click and answer the two questions)
rem ---------------------------------------------------------------------------------------------
set "HERE=%~dp0"
if /i "%~1"=="uninstall" goto :uninstall

set "GAME=%~1"
set "SLDIR=%~2"
if "%GAME%"=="" set /p "GAME=Game exe folder (e.g. D:\Games\Game\Game\Binaries\Win64) : "
if "%SLDIR%"=="" set /p "SLDIR=Folder with the Streamline DLLs (sl.interposer.dll ...)     : "
set "GAME=%GAME:"=%"
set "SLDIR=%SLDIR:"=%"

set "DLL=%HERE%winmm.dll"
if not exist "%DLL%" set "DLL=%HERE%src\winmm.dll"
if not exist "%DLL%" (echo [X] winmm.dll not found next to this script - build it first ^(src\build.sh^) or take it from the Releases page. & goto :fail)
if not exist "%GAME%\" (echo [X] Game folder not found: %GAME% & goto :fail)

set MISSING=0
for %%F in (sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll) do (
  if not exist "%SLDIR%\%%F" (echo [X] missing in the Streamline folder: %%F & set MISSING=1)
)
if "%MISSING%"=="1" (echo     Get them from the official Streamline SDK ^(bin\x64, production^). Nothing was copied. & goto :fail)

if exist "%GAME%\winmm.dll" if not exist "%GAME%\winmm.dll.bak" (
  findstr /c:"SN_DLSSG" "%GAME%\winmm.dll" >nul 2>&1 || (copy /y "%GAME%\winmm.dll" "%GAME%\winmm.dll.bak" >nul && echo [i] a foreign winmm.dll was saved as winmm.dll.bak)
)
copy /y "%DLL%" "%GAME%\winmm.dll" >nul || goto :fail
for %%F in (sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll) do copy /y "%SLDIR%\%%F" "%GAME%\%%F" >nul || goto :fail
if not exist "%GAME%\SN_DLSSG_cfg.txt" (
  if exist "%HERE%config\SN_DLSSG_cfg.txt" copy "%HERE%config\SN_DLSSG_cfg.txt" "%GAME%\SN_DLSSG_cfg.txt" >nul
  rem without a config file the DLL writes the right one itself on the first start
) else echo [i] existing SN_DLSSG_cfg.txt kept
echo.
echo [OK] installed into: %GAME%
echo      Start the game in DirectX 12 mode. Menu key: INSERT. Frame generation hotkey: F9.
echo      Uninstall:  install.bat uninstall "%GAME%"
exit /b 0

:uninstall
set "GAME=%~2"
if "%GAME%"=="" set /p "GAME=Game exe folder : "
set "GAME=%GAME:"=%"
if not exist "%GAME%\" (echo [X] Game folder not found: %GAME% & goto :fail)
for %%F in (winmm.dll sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll) do if exist "%GAME%\%%F" del /q "%GAME%\%%F"
if exist "%GAME%\winmm.dll.bak" ren "%GAME%\winmm.dll.bak" winmm.dll
echo [OK] removed. (Config, logs and caches SN_DLSSG_* were left in place - delete them by hand if you want.)
exit /b 0

:fail
echo.
pause
exit /b 1
