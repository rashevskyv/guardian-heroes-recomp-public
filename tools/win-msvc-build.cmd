@echo off
rem Windows x86-64 build: MSVC 2022 Build Tools (headers, libs, link.exe not needed) + clang with the MSVC target
rem (MSYS2 UCRT64 clang and lld, llvm-tools). Vulkan renderer only.
rem   tools\win-msvc-build.cmd [configure]     (run in a normal cmd; needs C:\proj\gh\sdk-msvc = the edited SDK)
rem SDK copy: WSL `python3 patches/sdk-edits-gh.py /root/gh/rexglue-sdk` then rsync to C:\proj\gh\sdk-msvc.
rem generated\default (codegen output) and game\default.xex must exist in the checkout (both git-ignored).
setlocal
call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1
set "PATH=C:\msys64\ucrt64\bin;%ProgramFiles%\Git\cmd;%PATH%"
if "%GH_SRC%"=="" (set SRC=%~dp0..) else (set SRC=%GH_SRC%)
if "%GH_BUILD%"=="" (set B=C:\proj\gh\build-msvc) else (set B=%GH_BUILD%)
set T=--target=x86_64-pc-windows-msvc
cmake -S "%SRC%" -B %B% -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER_TARGET=x86_64-pc-windows-msvc -DCMAKE_CXX_COMPILER_TARGET=x86_64-pc-windows-msvc ^
  -DCMAKE_C_FLAGS="-mssse3 -msse4.1" -DCMAKE_CXX_FLAGS="-mssse3 -msse4.1" -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld -DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld ^
  -DCMAKE_AR=C:/msys64/ucrt64/bin/llvm-ar.exe -DCMAKE_RC_COMPILER=C:/msys64/ucrt64/bin/llvm-rc.exe -DCMAKE_MT=C:/msys64/ucrt64/bin/llvm-mt.exe ^
  -DCMAKE_SYSTEM_PROCESSOR=AMD64 -DREXSDK_DIR=C:/proj/gh/sdk-msvc -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_USE_VULKAN=ON -DREXGLUE_USE_D3D12=OFF > C:\proj\gh\msvc-configure.log 2>&1 || (powershell -c "Get-Content C:\proj\gh\msvc-configure.log -Tail 30" & exit /b 1)
if /i "%1"=="configure" exit /b 0
cmake --build %B% --parallel 12 > C:\proj\gh\msvc-build.log 2>&1 || (powershell -c "Get-Content C:\proj\gh\msvc-build.log -Tail 30" & exit /b 1)
for /f %%i in ('git -C "%SRC%" rev-parse --short=7 HEAD') do echo %%i> %B%\built_commit.txt
echo BUILD_OK
