@echo off
cd /d "C:\Users\***\.zcode\workspace\default\projects\ProcessTopMonitor\plugin"
set MSVC=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
set SDK=C:\Program Files (x86)\Windows Kits\10
set SDKVER=10.0.26100.0

set COMMON_FLAGS=/nologo /O2 /EHsc /MT /std:c++17 /utf-8 /Zi /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WINDLL /Isrc
rem /OPT:REF must be explicit: /DEBUG defaults to NOREF and keeps all dead
rem code (2026-10-21 measured on PNM: x64 2.02MB -> 678KB, -67%)
set COMMON_LINK=/DLL /DEBUG /MAP /OPT:REF gdi32.lib user32.lib shell32.lib dwmapi.lib advapi32.lib tdh.lib comctl32.lib imm32.lib msimg32.lib pdh.lib

set "RC=%SDK%\bin\%SDKVER%\x86\rc.exe"
"%RC%" /nologo /fo src\resource.res src\resource.rc
if %ERRORLEVEL% NEQ 0 (
    echo RESOURCE COMPILE FAILED
    exit /b 1
)

echo === Building x64 ===
set "INCLUDE=%MSVC%\VC\Tools\MSVC\14.44.35207\include;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared"
set "LIB=%MSVC%\VC\Tools\MSVC\14.44.35207\lib\x64;%SDK%\Lib\%SDKVER%\um\x64;%SDK%\Lib\%SDKVER%\ucrt\x64"
set "PATH=%MSVC%\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64;%PATH%"
cl %COMMON_FLAGS% /FeProcessTopMonitor.dll src\metrics_collector.cpp src\etw_file_collector.cpp src\core_class_mapper.cpp src\gpu_collector.cpp src\top_aggregator.cpp src\top_list_window.cpp src\window_manager.cpp src\options_dialog.cpp src\plugin_main.cpp src\i18n.cpp /link %COMMON_LINK% src\resource.res /OUT:ProcessTopMonitor.dll
echo X64_EXIT=%ERRORLEVEL%

echo === Building x86 ===
set "INCLUDE=%MSVC%\VC\Tools\MSVC\14.44.35207\include;%SDK%\Include\%SDKVER%\um;%SDK%\Include\%SDKVER%\ucrt;%SDK%\Include\%SDKVER%\shared"
set "LIB=%MSVC%\VC\Tools\MSVC\14.44.35207\lib\x86;%SDK%\Lib\%SDKVER%\um\x86;%SDK%\Lib\%SDKVER%\ucrt\x86"
set "PATH=%MSVC%\VC\Tools\MSVC\14.44.35207\bin\Hostx86\x86;%PATH%"
cl %COMMON_FLAGS% /FeProcessTopMonitor_x86.dll src\metrics_collector.cpp src\etw_file_collector.cpp src\core_class_mapper.cpp src\gpu_collector.cpp src\top_aggregator.cpp src\top_list_window.cpp src\window_manager.cpp src\options_dialog.cpp src\plugin_main.cpp src\i18n.cpp /link %COMMON_LINK% src\resource.res /OUT:ProcessTopMonitor_x86.dll
echo X86_EXIT=%ERRORLEVEL%
