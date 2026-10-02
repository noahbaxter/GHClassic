@echo off
rem Run the game in a window, building it from the disc the first time.
rem Options: tools\ghc.py.
setlocal
set PY=python
where py >nul 2>nul && set PY=py -3
%PY% "%~dp0..\tools\ghc.py" play %*
