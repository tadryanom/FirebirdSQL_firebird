@echo off


:: Set env vars
@call setenvvar.bat %*

::==========
:: MAIN

@echo Extracting pre-built ICU
unzip -o %FB_ROOT_PATH%\extern\icu\icu_windows.zip -d %FB_ROOT_PATH%\extern\icu
unzip -o %FB_ROOT_PATH%\extern\icu\icudt.zip -d %FB_ROOT_PATH%\extern\icu
if errorlevel 1 call :ERROR build failed - see make_icu_%FB_TARGET_PLATFORM%.log for details

@echo Extracting tzdata
unzip -o %FB_ROOT_PATH%\extern\icu\tzdata\le.zip -d %FB_ROOT_PATH%\extern\icu\tzdata-extract

@goto :EOF


:ERROR
::====
@echo.
@echo   An error occurred while running make_icu.bat -
@echo     %*
@echo.
set ERRLEV=1
cancel_script > nul 2>&1
::End of ERROR
::------------
@goto :EOF
