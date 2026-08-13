@echo off
setlocal

if "%CAA_RADE_ROOT%"=="" (
  echo CAA_RADE_ROOT is required. Host tools must include intel_a\code\command.
  exit /b 2
)
if "%CAA_PREREQ_ROOT%"=="" (
  echo CAA_PREREQ_ROOT is required. win_b64 CAA prerequisites must already exist.
  exit /b 2
)
if not exist "%CAA_RADE_ROOT%\intel_a\code\command\MkmkSetenv.bat" (
  echo Missing RADE host command: %CAA_RADE_ROOT%\intel_a\code\command\MkmkSetenv.bat
  exit /b 3
)
if not exist "%CAA_RADE_ROOT%\intel_a\code\command\mkGetPreq.bat" (
  echo Missing RADE host command: %CAA_RADE_ROOT%\intel_a\code\command\mkGetPreq.bat
  exit /b 3
)
if not exist "%CAA_PREREQ_ROOT%\win_b64" (
  echo Missing prerequisite target directory: %CAA_PREREQ_ROOT%\win_b64
  exit /b 4
)

echo Host RADE tools: %CAA_RADE_ROOT%\intel_a\code\command
echo Target prerequisites: %CAA_PREREQ_ROOT%\win_b64
exit /b 0
