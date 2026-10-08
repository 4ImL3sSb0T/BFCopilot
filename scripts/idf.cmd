@echo off
rem Build wrapper for ESP-IDF v6.0.2 on this machine.
rem
rem Why this exists: MSYS2 (git-bash) re-injects MSYSTEM into native child processes,
rem and idf.py skips its main() entirely when it sees MSYSTEM. Running idf.py through
rem this .cmd clears MSYSTEM and sets the tool paths from the Espressif Installation
rem Manager profile.
rem
rem Usage (cmd):    scripts\idf.cmd build
rem Usage (bash):   cmd.exe //c "scripts\idf.cmd" build
rem The project directory is inferred from this script's location.

setlocal
set MSYSTEM=
set PROJECT_ROOT=%~dp0..
set IDF_PATH=C:\Users\yuang\Projects\Espidf\v6.0.2\esp-idf
set IDF_TOOLS_PATH=C:\Espressif\tools
set IDF_PYTHON_ENV_PATH=C:\Espressif\tools\python\v6.0.2\venv
set ESP_ROM_ELF_DIR=C:\Espressif\tools\esp-rom-elfs\20241011\
set OPENOCD_SCRIPTS=C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20260424\openocd-esp32\share\openocd\scripts
set ESP_CLANG_LIBS_PATH=C:\Espressif\tools\esp-clang-libs\esp-20.1.1_20250829\esp-clang\lib
set IDF_CCACHE_ENABLE=1
set ESP_IDF_VERSION=6.0
set PATH=C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\dfu-util\0.11\dfu-util-0.11-win64;C:\Espressif\tools\esp-clang\esp-20.1.1_20250829\esp-clang\bin;C:\Espressif\tools\esp-rom-elfs\20241011\;C:\Espressif\tools\esp32ulp-elf\2.38_20240113\esp32ulp-elf\bin;C:\Espressif\tools\idf-exe\1.0.3\;C:\Espressif\tools\ninja\1.12.1\;C:\Espressif\tools\openocd-esp32\v0.12.0-esp32-20260424\openocd-esp32\bin;C:\Espressif\tools\riscv32-esp-elf-gdb\17.1_20260402\riscv32-esp-elf-gdb\bin;C:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\riscv32-esp-elf\bin;C:\Espressif\tools\xtensa-esp-elf-gdb\17.1_20260402\xtensa-esp-elf-gdb\bin;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\python\v6.0.2\venv\Scripts;%PATH%

cd /D "%PROJECT_ROOT%"
python.exe "%IDF_PATH%\tools\idf.py" %*
endlocal
