@echo off
rem Plays Chains of Olympus in a window with the native port (build/cmake_full).
rem Extra arguments are passed on, e.g.:  tools\play.cmd --fullscreen   or   tools\play.cmd --scale 3
rem Keys: arrows = D-pad, Z = cross, X = circle, A = square, S = triangle, Q/W = L/R,
rem       Space/Enter = Start, V = Select, I/J/K/L = analog stick, F11 = fullscreen, F12 = screenshot, Esc = quit.
setlocal
cd /d "%~dp0.."
set EXE=build\cmake_full\Release\gow_rt_UCUS98653_full.exe
if not exist "%EXE%" (
  echo %EXE% not found: build it first, see recomp\README.md
  exit /b 1
)
if not exist media\screenshots mkdir media\screenshots
"%EXE%" --image build\gen\UCUS98653_full\image.bin --boot --disc "God of War - Chains of Olympus (USA).chd" --ms0 build\ms0 --window --shots media\screenshots %* > build\play_last.log 2>&1
echo Log: build\play_last.log
