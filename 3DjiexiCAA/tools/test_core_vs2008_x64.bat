@echo off
setlocal

if "%VS90COMNTOOLS%"=="" (
  echo VS90COMNTOOLS is required.
  exit /b 2
)

set "CADPARSE_ROOT=%~dp0.."
set "CADPARSE_SRC=%CADPARSE_ROOT%\CadParseMvp.edu\CadParseMvp.m\src"
set "CADPARSE_BUILD=%CADPARSE_ROOT%\build_core_x64"
if not exist "%CADPARSE_BUILD%" mkdir "%CADPARSE_BUILD%"

set "VCVARSALL=%VS90COMNTOOLS%..\..\VC\vcvarsall.bat"
if not exist "%VCVARSALL%" (
  echo VS2008 vcvarsall.bat is required.
  exit /b 3
)
call "%VCVARSALL%" amd64
if errorlevel 1 call "%VCVARSALL%" x86_amd64
if errorlevel 1 exit /b 4

cl /nologo /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I"%CADPARSE_SRC%" /c "%CADPARSE_SRC%\CadParseCore.cpp" /Fo"%CADPARSE_BUILD%\CadParseCore.obj"
if errorlevel 1 exit /b 5
cl /nologo /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I"%CADPARSE_SRC%" /c "%CADPARSE_SRC%\CadParseIR.cpp" /Fo"%CADPARSE_BUILD%\CadParseIR.obj"
if errorlevel 1 exit /b 5
cl /nologo /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I"%CADPARSE_SRC%" /c "%CADPARSE_SRC%\CadParseSelfTests.cpp" /Fo"%CADPARSE_BUILD%\CadParseSelfTests.obj"
if errorlevel 1 exit /b 5
cl /nologo /EHsc /W4 /D_CRT_SECURE_NO_WARNINGS /I"%CADPARSE_SRC%" /c "%CADPARSE_ROOT%\tests\CadParseCoreTestMain.cpp" /Fo"%CADPARSE_BUILD%\CadParseCoreTestMain.obj"
if errorlevel 1 exit /b 5
link /nologo "%CADPARSE_BUILD%\CadParseCore.obj" "%CADPARSE_BUILD%\CadParseIR.obj" "%CADPARSE_BUILD%\CadParseSelfTests.obj" "%CADPARSE_BUILD%\CadParseCoreTestMain.obj" /OUT:"%CADPARSE_BUILD%\CadParseCoreTests.exe"
if errorlevel 1 exit /b 6

pushd "%CADPARSE_BUILD%"
CadParseCoreTests.exe
set "CADPARSE_RESULT=%errorlevel%"
popd
exit /b %CADPARSE_RESULT%
