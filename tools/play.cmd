@echo off
rem Plays Chains of Olympus in a window with the native port (build/cmake_full).
rem Extra arguments are passed on, e.g.:  tools\play.cmd --fullscreen   or   tools\play.cmd --scale 3
rem Keys: arrows = D-pad, Z = cross, X = circle, A = square, S = triangle, Q/W = L/R,
rem       Space/Enter = Start, V = Select, I/J/K/L = analog stick, F11 = fullscreen, F12 = screenshot, Esc = quit.
rem Sound plays through the default audio device; --mute turns it off.
rem Saves go to build\ms0\PSP\SAVEDATA (see docs\phase9-saves-replay.md).
rem Every session is recorded so that it can be replayed, e.g. up to a crash: build\sessions\<date_time>\
rem holds input.txt (what the game read from the controller), ms0_start (the saves as they were when
rem the session started), args.txt, build.txt (which exe played it: replays need the same build) and
rem run.log. Replay one with:  tools\replay.cmd build\sessions\<date_time>
setlocal
cd /d "%~dp0.."
set EXE=build\cmake_full\Release\gow_rt_UCUS98653_full.exe
if not exist "%EXE%" (
  echo %EXE% not found: build it first, see recomp\README.md
  exit /b 1
)
if not exist media\screenshots mkdir media\screenshots
if not exist build\ms0 mkdir build\ms0
for /f %%t in ('powershell -NoProfile -Command "Get-Date -Format yyyy-MM-dd_HHmmss"') do set STAMP=%%t
set SES=build\sessions\%STAMP%
mkdir "%SES%"
robocopy build\ms0 "%SES%\ms0_start" /E /COPY:DAT /DCOPY:T /NFL /NDL /NJH /NJS /NP >nul
echo %*> "%SES%\args.txt"
for %%f in ("%EXE%") do echo %%~nxf %%~tf %%~zf bytes> "%SES%\build.txt"
"%EXE%" --image build\gen\UCUS98653_full\image.bin --boot --disc "God of War - Chains of Olympus (USA).chd" --ms0 build\ms0 --window --shots media\screenshots --record-input "%SES%\input.txt" %* > build\play_last.log 2>&1
copy /y build\play_last.log "%SES%\run.log" >nul
echo Log: build\play_last.log
echo Session: %SES%  (replay: tools\replay.cmd %SES%)
