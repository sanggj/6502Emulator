@echo off
setlocal
if not exist build mkdir build
set SRCS=src\crash_handler.cpp src\platform.cpp src\apu.cpp src\assembler.cpp src\capture.cpp src\cpu.cpp src\debugger.cpp src\debug_console.cpp src\display.cpp src\emulator.cpp src\feature_catalog.cpp src\hexfile.cpp src\ines.cpp src\memory.cpp src\remote.cpp src\savestate.cpp src\serial.cpp src\strings.cpp src\theme.cpp src\settings.cpp src\win_util.cpp src\code_editor.cpp src\code_editor_input.cpp src\code_editor_keys.cpp src\hex_view.cpp src\disasm_view.cpp src\regs_view.cpp src\term_view.cpp src\app_actions.cpp src\app_terminal.cpp src\headless.cpp src\main_ui.cpp src\selftest_roms.cpp src\main.cpp
windres app.rc -O coff -o build\app_rc.o
if errorlevel 1 exit /b 1
clang++ -std=c++17 -O2 -Wall -Wextra -Werror -Iinclude %SRCS% build\app_rc.o -o build\6502_ide.exe -mwindows -luser32 -lgdi32 -lcomdlg32 -lcomctl32 -lshell32 -lkernel32 -lws2_32 -lwinmm -ldbghelp
if errorlevel 1 exit /b 1
echo Built build\6502_ide.exe

clang++ -std=c++17 -O1 -Wall -Wextra -Iinclude src\assembler.cpp src\cpu.cpp src\memory.cpp src\hexfile.cpp src\savestate.cpp src\emulator.cpp src\apu.cpp src\capture.cpp src\ines.cpp src\selftest_roms.cpp src\debugger.cpp src\strings.cpp src\platform.cpp src\settings.cpp src\remote.cpp tests\test_asm.cpp tests\test_cpu.cpp tests\test_platform.cpp tests\test_core.cpp -o build\test_core.exe -mconsole -lgdi32 -luser32 -lwinmm -lws2_32 -lcomdlg32
if errorlevel 1 exit /b 1
build\test_core.exe
