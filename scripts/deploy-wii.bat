@echo off
setlocal EnableExtensions

:: Assemble the Homebrew Channel app folder (boot.dol + meta.xml + VC assets).
:: Usage:
::   scripts\deploy-wii.bat E:            -> install boot.dol/meta.xml to E:\apps\reVC
::   scripts\deploy-wii.bat E: assets     -> also copy the Vice City PC assets (first run)
::   scripts\deploy-wii.bat E: root       -> data in E:\reVC-data, app in E:\apps\reVC
::   scripts\deploy-wii.bat E: root assets-> first run, data in E:\reVC-data
::   scripts\deploy-wii.bat local         -> stage everything to .\dist\reVC on disk

set "REPO=%~dp0.."
set "BUILD_DOL=%REPO%\build-wii\src\reVC.dol"
set "VC_DIR=D:\Program Files (x86)\Rockstar Games\Grand Theft Auto Vice City"

set "TARGET=%~1"
set "ASSETS=%~2"
set "ROOTMODE=0"
if /I "%~2"=="root" (
    set "ROOTMODE=1"
    set "ASSETS=%~3"
)
if /I "%TARGET%"=="local" ( set "TARGET=%REPO%\dist" & set "MODE=local" )

if "%TARGET%"=="" (
    echo.
    echo Insert your SD/USB card, then run, e.g.:
    echo   scripts\deploy-wii.bat E:          ^(app only; reuse existing assets^)
    echo   scripts\deploy-wii.bat E: assets   ^(first time: full copy, several minutes^)
    echo   scripts\deploy-wii.bat E: root     ^(data in reVC-data, app in apps\reVC^)
    echo   scripts\deploy-wii.bat local       ^(stage to .\dist on disk for Dolphin^)
    echo.
    echo Removable drives detected:
    powershell -NoProfile -Command "Get-CimInstance Win32_LogicalDisk -Filter \"DriveType=2 or DriveType=3\" | Select-Object DeviceID,VolumeName,Size"
    exit /b 1
)

set "APP=%TARGET%\apps\reVC"
set "DATA=%APP%"
if "%ROOTMODE%"=="1" set "DATA=%TARGET%\reVC-data"

if not exist "%BUILD_DOL%" (
    echo ERROR: missing %BUILD_DOL%. Build first.
    exit /b 1
)

echo.
echo [1/3] Creating %APP%
if not exist "%APP%" mkdir "%APP%" || goto :fail
if not exist "%DATA%" mkdir "%DATA%" || goto :fail

echo [2/3] Copying boot.dol, meta.xml, icon
copy /Y "%BUILD_DOL%" "%APP%\boot.dol" >nul || goto :fail
copy /Y "%REPO%\gamefiles\wii-hbc\meta.xml" "%APP%\meta.xml" >nul || goto :fail
copy /Y "%REPO%\gamefiles\wii-hbc\icon.png" "%APP%\icon.png" >nul || goto :fail
:: The TTF/OTF under gamefiles\wii-hbc\fonts are only used on the PC by
:: make-wii-icon.ps1 to render icon.png.  The game has no reference to them,
:: so they are not deployed.

if /I "%ASSETS%"=="assets" (
    echo [3/3] Copying Vice City PC assets from:
    echo      "%VC_DIR%"
    echo    Press Ctrl+C now if this is not what you want.
    for %%D in (anim data models TEXT mp3 audio txd skins) do (
        if exist "%VC_DIR%\%%D" (
            echo    - %%D
            if not exist "%DATA%\%%D" mkdir "%DATA%\%%D"
            xcopy /E /I /Y /Q "%VC_DIR%\%%D" "%DATA%\%%D" >nul
        )
    )
    echo    - movies skipped on purpose ^(edit this script to add them^)
) else (
    echo [3/3] Assets: untouched ^(run with the word "assets" to copy them^)
)

echo [4/4] Overwriting with repository gamefiles ^(compat fixes, GXT, neo^)
xcopy /E /I /Y /Q "%REPO%\gamefiles\data" "%DATA%\data" >nul
xcopy /E /I /Y /Q "%REPO%\gamefiles\models" "%DATA%\models" >nul
xcopy /E /I /Y /Q "%REPO%\gamefiles\TEXT" "%DATA%\TEXT" >nul
if exist "%REPO%\gamefiles\neo" xcopy /E /I /Y /Q "%REPO%\gamefiles\neo" "%DATA%\neo" >nul

echo.
echo [5/5] Syncing boot.dol to every other install we can see ^(SD/USB/^Dolphin^)
:: Stale-DOL guard: the Wii only runs what the SD card carries.  Sweep all
:: connected drives and update every existing apps\reVC install so no device
:: can be left with an old boot.dol after a rebuild.
if /I "%MODE%"=="local" goto :synced
for %%D in (A B C D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if exist "%%D:\apps\reVC\boot.dol" (
        if /I not "%%D:"=="%TARGET%" (
            echo    - %%D:\apps\reVC
            copy /Y "%BUILD_DOL%" "%%D:\apps\reVC\boot.dol" >nul
            copy /Y "%REPO%\gamefiles\wii-hbc\meta.xml" "%%D:\apps\reVC\meta.xml" >nul
            copy /Y "%REPO%\gamefiles\wii-hbc\icon.png" "%%D:\apps\reVC\icon.png" >nul
        )
    )
)
echo    - %REPO%\dist\sd\apps\reVC
if not exist "%REPO%\dist\sd\apps\reVC" mkdir "%REPO%\dist\sd\apps\reVC"
copy /Y "%BUILD_DOL%" "%REPO%\dist\sd\apps\reVC\boot.dol" >nul
:synced

echo.
echo Done. Every connected install now carries the current build.
exit /b 0

:fail
echo ERROR: copy failed.
exit /b 1

