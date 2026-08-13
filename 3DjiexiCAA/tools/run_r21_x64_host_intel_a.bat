@echo off
setlocal

call "%~dp0probe_r21_x64_existing_env.bat"
if errorlevel 1 exit /b %errorlevel%

set "_MkmkOS_BitMode=64"
set "MkmkOS_BitMode=64"
set "MkmkINSTALL_PATH=%CAA_RADE_ROOT%"
set "CADPARSE_WORKSPACE=%~dp0.."
set "CADPARSE_EXE=%CADPARSE_WORKSPACE%\win_b64\code\bin\CadParseMvp.exe"

call "%CAA_RADE_ROOT%\intel_a\code\command\MkmkSetenv.bat" >nul
if errorlevel 1 exit /b 3
if not exist "%CADPARSE_EXE%" (
  echo Parser executable not found. Run build_r21_x64_host_intel_a.bat on a complete win_b64 prerequisite machine first.
  exit /b 4
)

set "PATH=%CADPARSE_WORKSPACE%\win_b64\code\bin;%CAA_PREREQ_ROOT%\win_b64\code\bin;%PATH%"
"%CADPARSE_EXE%" %*
exit /b %errorlevel%
