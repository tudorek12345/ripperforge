@echo off
setlocal
pushd "%~dp0"

rem Configure Detours paths if not provided by the environment
if not defined DETOURS_INCLUDE set "DETOURS_INCLUDE=C:\detours\include"
if not defined DETOURS_LIB set "DETOURS_LIB=C:\detours\lib.X64"

if "%DLL_NAME%"=="" set "DLL_NAME=ripper_new6.dll"

rem Build injector (exe)
cl /nologo /std:c++17 /EHsc /MD AssetRipper.cpp ^
    /I"%DETOURS_INCLUDE%" ^
    /link user32.lib gdi32.lib comdlg32.lib psapi.lib shell32.lib ole32.lib

rem Build hook DLL
cl /nologo /std:c++17 /EHsc /MD /LD ripper.cpp ^
    /I"%DETOURS_INCLUDE%" ^
    /link detours.lib d3d9.lib d3d11.lib dxgi.lib dxguid.lib user32.lib gdi32.lib windowscodecs.lib ole32.lib ^
    /OUT:%DLL_NAME% ^
    /LIBPATH:"%DETOURS_LIB%"

popd
endlocal
