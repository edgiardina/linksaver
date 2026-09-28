@echo off
REM Builds linksaver.scr (and linksaver.exe, the same binary) with TCC.
REM Prereqs are the same as run_with_tcc.bat: third_party\tcc and third_party\SDL2-2.26.3.
setlocal
cd /d "%~dp0"

set SDL2=third_party\SDL2-2.26.3

IF NOT EXIST "third_party\tcc\tcc.exe" (
  ECHO ERROR: third_party\tcc\tcc.exe missing. See README.md.
  EXIT /B 1
)
IF NOT EXIST "%SDL2%\lib\x64\SDL2.dll" (
  ECHO ERROR: SDL2 missing from %SDL2%. See README.md.
  EXIT /B 1
)

echo Building linksaver with TCC...
third_party\tcc\tcc.exe -olinksaver.exe -DCOMPILER_TCC=1 -DSTBI_NO_SIMD=1 -DHAVE_STDINT_H=1 -D_HAVE_STDINT_H=1 -DSYSTEM_VOLUME_MIXER_AVAILABLE=0 -I%SDL2%/include -L%SDL2%/lib/x64 -lSDL2 -luser32 -I. src/*.c src/linksaver/*.c snes/*.c third_party/gl_core/gl_core_3_1.c third_party/opus-1.3.1-stripped/opus_decoder_amalgam.c
IF ERRORLEVEL 1 EXIT /B 1

copy /y %SDL2%\lib\x64\SDL2.dll . >nul
copy /y linksaver.exe linksaver.scr >nul
echo Built linksaver.exe and linksaver.scr

REM Single-file build: a small launcher with everything else appended.
third_party\tcc\tcc.exe -olinksaver_launcher.exe -Wl,-subsystem=windows -luser32 -lgdi32 src/launcher/launcher.c src/launcher/settings_dialog.c
IF ERRORLEVEL 1 EXIT /B 1
IF EXIST zelda3_assets.dat (
  python tools\pack_linksaver.py
) ELSE (
  echo Skipping dist\Linksaver.scr: extract zelda3_assets.dat first.
)
