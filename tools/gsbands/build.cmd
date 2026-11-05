@echo off
rem Builds gsbands.exe (banded GS rasterizer harness, see docs/gs-bands.md).
rem Usage (from ghost_pc):  tools\gsbands\build.cmd [bandsonly]
rem Output: work\gsbands\gsbands.exe (+ objects). Same compiler flags as out\build_perf.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set H=%~dp0
set G=%H%..\..
set R=%G%\work\ps2recomp_game\ps2xRuntime
set O=%G%\work\gsbands
set FLAGS=/nologo /c /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG -std:c++20 -MD /Oi /Gy /Gw /GF /Zc:inline /fp:fast /arch:AVX2 /GS- /Qspectre- /Zi -DGRAPHICS_API_OPENGL_33 -DPLATFORM_DESKTOP /W0
set INC=/I%R%\include /I%R%\src\lib\Kernel /I%R%\..\ps2xIOP\include
if not exist "%O%" mkdir "%O%"
cd /d "%O%"
if "%~1"=="bandsonly" goto bands
cl %FLAGS% %INC% "%R%\src\lib\gs\gs_cpu_backend.cpp" /Focpu_backend.obj || exit /b 1
cl %FLAGS% %INC% "%R%\src\lib\gs\ps2_gs_memory.cpp" /Fomem.obj || exit /b 1
cl %FLAGS% %INC% "%R%\src\lib\gs\gs_feature_stats.cpp" /Fofeature.obj || exit /b 1
:bands
cl %FLAGS% %INC% "%R%\src\lib\gs\gs_banded_backend.cpp" /Fobanded.obj || exit /b 1
cl %FLAGS% %INC% "%H%main.cpp" /Fomain.obj || exit /b 1
link /nologo /DEBUG /OPT:REF /OPT:ICF /OUT:gsbands.exe main.obj banded.obj cpu_backend.obj mem.obj feature.obj || exit /b 1
echo GSBANDS OK: %O%\gsbands.exe
