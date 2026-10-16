@echo off
rem Plays Ghost of Sparta in a window with the native port (build/cmake_gos_full), on the hardware renderer.
rem Extra arguments are passed on, e.g.:  tools\play_gos.cmd --fullscreen   or   tools\play_gos.cmd --renderer soft
rem Keys: arrows = D-pad, Z = cross, X = circle, A = square, S = triangle, Q/W = L/R,
rem       Space/Enter = Start, V = Select, I/J/K/L = analog stick, F11 = fullscreen, F12 = screenshot, Esc = quit.
rem Sound plays through the default audio device; --mute turns it off.
rem Saves go to build\ms0_gos\PSP\SAVEDATA (Chains of Olympus keeps build\ms0; see docs\phase10-ghost-of-sparta.md).
rem Every session is recorded like with tools\play.cmd, in build\sessions\<date_time>\ (input.txt, ms0_start,
rem args.txt, build.txt, run.log); tools\replay.cmd tells the two games apart by build.txt:
rem   tools\replay.cmd build\sessions\<date_time> --renderer gpu
setlocal
cd /d "%~dp0.."
set EXE=build\cmake_gos_full\Release\gow_rt_UCUS98737_full.exe
if not exist "%EXE%" (
  echo %EXE% not found: build it first, see recomp\README.md
  exit /b 1
)
if not exist media\screenshots mkdir media\screenshots
if not exist build\ms0_gos mkdir build\ms0_gos
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd_HHmmss"') do set STAMP=%%t
set SES=build\sessions\%STAMP%
mkdir "%SES%"
robocopy build\ms0_gos "%SES%\ms0_start" /E /COPY:DAT /DCOPY:T /NFL /NDL /NJH /NJS /NP >nul
echo --renderer gpu %*> "%SES%\args.txt"
for %%f in ("%EXE%") do echo %%~nxf %%~tf %%~zf bytes> "%SES%\build.txt"
"%EXE%" --image build\gen\UCUS98737_full\image.bin --boot --disc "God of War - Ghost of Sparta (USA) (En,Fr,Es).chd" --ms0 build\ms0_gos --window --renderer gpu --shots media\screenshots --record-input "%SES%\input.txt" %* > build\play_gos_last.log 2>&1
copy /y build\play_gos_last.log "%SES%\run.log" >nul
echo Log: build\play_gos_last.log
echo Session: %SES%  (replay: tools\replay.cmd %SES%)
