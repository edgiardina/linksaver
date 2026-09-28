// Windows screensaver shell (screensaver.c). Kept free of types.h so that
// screensaver.c can include windows.h, whose names collide with types.h macros.
#ifndef ZELDA3_LINKSAVER_SCREENSAVER_H_
#define ZELDA3_LINKSAVER_SCREENSAVER_H_

#include <stdbool.h>
#include <stdint.h>

typedef enum ScreensaverMode {
  kScrMode_None,       // Plain game (no screensaver arguments)
  kScrMode_Demo,       // --linksaver: autopilot in a normal window, input doesn't exit
  kScrMode_Fullscreen, // /s
  kScrMode_Preview,    // /p <hwnd>
  kScrMode_Configure,  // /c or .scr launched with no args
} ScreensaverMode;

extern ScreensaverMode g_scr_mode;

// Consumes screensaver arguments. Returns the number of argv entries used.
int Screensaver_ParseArgs(int argc, char **argv);
void Screensaver_ChdirToExe();
void Screensaver_ShowConfigure();
// Returns an SDL_Window* for preview mode, parented to the Control Panel HWND.
void *Screensaver_CreatePreviewWindow(uint32_t sdl_flags);
bool Screensaver_PreviewParentAlive();
// Covers secondary monitors with black windows in fullscreen mode.
void Screensaver_BlankOtherDisplays(int primary_display);
void Screensaver_DestroyBlankWindows();
// Returns true if this input event should end the screensaver.
bool Screensaver_ShouldExitOnEvent(const void *sdl_event);

#endif  // ZELDA3_LINKSAVER_SCREENSAVER_H_
