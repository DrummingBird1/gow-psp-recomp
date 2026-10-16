@echo off
rem Replays a session recorded by tools\play.cmd headless and deterministically: from the saves the
rem session started with (a fresh copy of ms0_start) to the frame where it stopped or crashed.
rem Extra arguments are passed on, e.g. to look at the last minute before a crash at frame 41000:
rem   tools\replay.cmd build\sessions\2026-10-17_201500 --fast-forward 37000 --frames-dir build\rp --frames-every 30
rem --fast-forward N draws nothing before frame N (the game itself runs exactly the same, about twice as fast).
rem A session played with --renderer gpu gives the same game either way; pass --renderer gpu to compare frames.
setlocal
cd /d "%~dp0.."
set EXE=build\cmake_full\Release\gow_rt_UCUS98653_full.exe
set SES=%~1
if "%SES%"=="" goto usage
if not exist "%SES%\input.txt" goto usage
set ARGS=
:args
shift
if "%~1"=="" goto run
set ARGS=%ARGS% %1
goto args
:run
if exist "%SES%\replay_ms0" rmdir /s /q "%SES%\replay_ms0"
robocopy "%SES%\ms0_start" "%SES%\replay_ms0" /E /COPY:DAT /DCOPY:T /NFL /NDL /NJH /NJS /NP >nul
"%EXE%" --image build\gen\UCUS98653_full\image.bin --boot --disc "God of War - Chains of Olympus (USA).chd" --ms0 "%SES%\replay_ms0" --input "%SES%\input.txt" --cpu-ms 4000000000 --wall-ms 4000000000 %ARGS% > "%SES%\replay.log" 2>&1
echo Log: %SES%\replay.log
exit /b 0
:usage
echo usage: tools\replay.cmd build\sessions\DATE_TIME [extra arguments]
exit /b 1
