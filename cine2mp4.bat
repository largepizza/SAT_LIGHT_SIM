@echo off
rem Cinematic export frames -> MP4 (H.264, CRF 18) via tools\harness\frames2video.py.
rem   cine2mp4                  the newest folder in screenshots\cinematics
rem   cine2mp4 <folder> [...]   those folders (or drag folders onto this file)
rem Writes <folder>.mp4 next to each folder, at the fps in its cinematic.json. Needs ffmpeg on PATH.
rem Works from anywhere inside the repo (e.g. a copy in screenshots\cinematics): it finds the repo root
rem by walking up from its own folder.
setlocal
set "ROOT=%~dp0"
:findroot
if exist "%ROOT%tools\harness\frames2video.py" goto :found
for %%P in ("%ROOT%..") do set "UP=%%~fP\"
if /i "%UP%"=="%ROOT%" (
    echo Could not find tools\harness\frames2video.py above %~dp0
    goto :done
)
set "ROOT=%UP%"
goto :findroot
:found
if "%~1"=="" (
    for /f "delims=" %%D in ('dir /b /ad /o-d "%ROOT%screenshots\cinematics" 2^>nul') do (
        call :one "%ROOT%screenshots\cinematics\%%D"
        goto :done
    )
    echo No export folders in %ROOT%screenshots\cinematics.
    goto :done
)
:loop
if "%~1"=="" goto :done
call :one "%~1"
shift
goto :loop
:one
echo %~1
python "%ROOT%tools\harness\frames2video.py" "%~1"
exit /b
:done
if not defined CINE2MP4_NOPAUSE pause
endlocal
