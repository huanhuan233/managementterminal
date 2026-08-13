@echo off
setlocal

call "%~dp0probe_r21_x64_existing_env.bat"
if errorlevel 1 exit /b %errorlevel%

set "_MkmkOS_BitMode=64"
set "MkmkOS_BitMode=64"
set "MkmkINSTALL_PATH=%CAA_RADE_ROOT%"
set "CADPARSE_WORKSPACE=%~dp0.."
set "CADPARSE_LOG=%CADPARSE_WORKSPACE%\build_r21_x64.log"
set "CADPARSE_EXE=%CADPARSE_WORKSPACE%\win_b64\code\bin\CadParseMvp.exe"

set "VCVARSALL=%VS90COMNTOOLS%..\..\VC\vcvarsall.bat"
if not exist "%VCVARSALL%" (
  echo VS2008 vcvarsall.bat is required for x64 host compilation.
  exit /b 5
)
call "%VCVARSALL%" amd64
if errorlevel 1 call "%VCVARSALL%" x86_amd64
if errorlevel 1 exit /b 6

call "%CAA_RADE_ROOT%\intel_a\code\command\MkmkSetenv.bat"
if errorlevel 1 exit /b 7

call "%CAA_RADE_ROOT%\intel_a\code\command\mkGetPreq.bat" -W "%CADPARSE_WORKSPACE%" -p "%CAA_PREREQ_ROOT%"
if errorlevel 1 (
  echo Missing or invalid win_b64 CAA prerequisite. JDK, Fortran, or VSTA warnings are not the primary success criterion.
  exit /b 8
)

if exist "%CADPARSE_EXE%" del /q "%CADPARSE_EXE%"
call "%CAA_RADE_ROOT%\intel_a\code\command\mkmk.bat" -W "%CADPARSE_WORKSPACE%" CadParseMvp.edu CadParseMvp.m -a win_b64 -jobs 1 -w > "%CADPARSE_LOG%" 2>&1
set "CADPARSE_MKMK_RESULT=%errorlevel%"
type "%CADPARSE_LOG%"

if not "%CADPARSE_MKMK_RESULT%"=="0" exit /b 9
findstr /C:"# make-ERROR" /C:"# mkmk-ERROR" /C:"# syst-ERROR" /C:": error C" /C:"fatal error" /C:"error LNK" "%CADPARSE_LOG%" >nul
if not errorlevel 1 exit /b 9
if not exist "%CADPARSE_EXE%" (
  echo Missing expected x64 output: %CADPARSE_EXE%
  exit /b 10
)

echo Build succeeded: %CADPARSE_EXE%
exit /b 0
