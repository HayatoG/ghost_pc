@echo off
rem Builds gsreplay.exe (GS replay harness, see docs/gs-raster-cpu.md and docs/gs-hw-backend.md).
rem Usage (from ghost_pc):  tools\gsreplay\build.cmd [baseline_dir] [newonly|hwonly]
rem   baseline_dir: frozen copy of the rasterizer sources used as reference
rem                 (layout: runtime\gs\*.h and src\*.cpp); default work\raster_baseline_src
rem   newonly: rebuild only the working-tree CPU backend (+ hw); hwonly: rebuild only the hardware renderer
rem Output: work\gsreplay\gsreplay.exe (+ objects). Same compiler flags as out\build_perf.
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set H=%~dp0
set G=%H%..\..
set S=%~1
if "%S%"=="" set S=%G%\work\raster_baseline_src
set R=%G%\work\ps2recomp_game\ps2xRuntime
set O=%G%\work\gsreplay
set FLAGS=/nologo /c /DWIN32 /D_WINDOWS /EHsc /O2 /Ob2 /DNDEBUG -std:c++20 -MD /Oi /Gy /Gw /GF /Zc:inline /fp:fast /arch:AVX2 /GS- /Qspectre- /Zi -DGRAPHICS_API_OPENGL_33 -DPLATFORM_DESKTOP -DPS2X_HAS_GS_GPU=1 /W0
set INC=/I%R%\include /I%R%\src\lib\Kernel /I%R%\..\ps2xIOP\include
set OLD=/I%S% %INC% /DGSMem=GSMemOld /DGSCpuBackend=GSCpuBackendOld
if not exist "%O%" mkdir "%O%"
cd /d "%O%"
if "%~2"=="newonly" goto new
if "%~2"=="hwonly" goto hw
cl %FLAGS% %OLD% "%S%\src\gs_cpu_backend.cpp" /Foold_backend.obj || exit /b 1
cl %FLAGS% %OLD% "%S%\src\ps2_gs_memory.cpp" /Foold_mem.obj || exit /b 1
cl %FLAGS% %OLD% /DFACTORY_SUFFIX=Old "%H%factory.cpp" /Foold_factory.obj || exit /b 1
cl %FLAGS% %INC% "%R%\src\lib\gs\gs_feature_stats.cpp" /Fofeature.obj || exit /b 1
:new
cl %FLAGS% %INC% "%H%main.cpp" /Fomain.obj || exit /b 1
cl %FLAGS% %INC% "%R%\src\lib\gs\gs_cpu_backend.cpp" /Fonew_backend.obj || exit /b 1
cl %FLAGS% %INC% "%R%\src\lib\gs\ps2_gs_memory.cpp" /Fonew_mem.obj || exit /b 1
cl %FLAGS% %INC% /DFACTORY_SUFFIX=New "%H%factory.cpp" /Fonew_factory.obj || exit /b 1
:hw
cl %FLAGS% %INC% /I%R%\src\lib\gs "%R%\src\lib\gs\gs_hw_backend.cpp" /Fohw_backend.obj || exit /b 1
cl %FLAGS% %INC% "%R%\src\lib\gs\gs_rhi_hw_d3d11.cpp" /Fohw_rhi.obj || exit /b 1
cl %FLAGS% %INC% "%H%hw_tools.cpp" /Fohw_tools.obj || exit /b 1
link /nologo /DEBUG /OPT:REF /OPT:ICF /OUT:gsreplay.exe main.obj feature.obj old_backend.obj old_mem.obj old_factory.obj new_backend.obj new_mem.obj new_factory.obj hw_backend.obj hw_rhi.obj hw_tools.obj d3d11.lib dxgi.lib d3dcompiler.lib user32.lib || exit /b 1
echo GSREPLAY OK: %O%\gsreplay.exe
