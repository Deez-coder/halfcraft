@echo off
rem Builds hs\build\HSPassthrough.addon64, the ReShade add-on (and engine hooks) for Half Sword.
rem Needs third_party\: reshade\ headers, sdk\ (Dumper-7 CppSDK for this game build, enum sizes fixed by
rem lab\fix_enums.py), minhook\.
setlocal
cd /d "%~dp0"
rem (no for /f here: the ")" in "Program Files (x86)" ends a parenthesised block)
if not "%VCVARS%"=="" goto havevc
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvars64.bat > "%TEMP%\hsp_vcvars.txt"
set /p VCVARS=<"%TEMP%\hsp_vcvars.txt"
:havevc
rem a stray quote in a PATH entry breaks vcvars64.bat: drop quotes for this session
set "PATH=%PATH:"=%"
call "%VCVARS%" >nul || exit /b 1
if not exist build\obj mkdir build\obj
set CFLAGS=/nologo /O2 /MD /EHsc /std:c++latest /bigobj /W3 /wd4996 /wd4244 /wd4267 /wd4005 /wd4369 /wd4309 /DNOMINMAX /I third_party\reshade /I third_party\sdk /I third_party\minhook\include /Fobuild\obj\
rem the SDK sources change only with a new game build: compile them once
if not exist build\obj\Engine_functions.obj (
	cl %CFLAGS% /c third_party\sdk\SDK\Basic.cpp third_party\sdk\SDK\CoreUObject_functions.cpp third_party\sdk\SDK\Engine_functions.cpp || exit /b 1
)
if not exist build\obj\Willie_BP_functions.obj (
	cl %CFLAGS% /c third_party\sdk\SDK\Willie_BP_functions.cpp || exit /b 1
)
if not exist build\obj\hde64.obj (
	cl /nologo /O2 /MD /c /I third_party\minhook\include /Fobuild\obj\ third_party\minhook\src\buffer.c third_party\minhook\src\hook.c third_party\minhook\src\trampoline.c third_party\minhook\src\hde\hde64.c || exit /b 1
)
cl %CFLAGS% /c src\main.cpp src\bridge.cpp src\input.cpp src\compositor.cpp src\ws.cpp src\log.cpp || exit /b 1
link /nologo /DLL /OUT:build\HSPassthrough.addon64 build\obj\main.obj build\obj\bridge.obj build\obj\input.obj build\obj\compositor.obj build\obj\ws.obj build\obj\log.obj ^
	build\obj\Basic.obj build\obj\CoreUObject_functions.obj build\obj\Engine_functions.obj build\obj\Willie_BP_functions.obj ^
	build\obj\buffer.obj build\obj\hook.obj build\obj\trampoline.obj build\obj\hde64.obj ws2_32.lib user32.lib || exit /b 1
echo built build\HSPassthrough.addon64
