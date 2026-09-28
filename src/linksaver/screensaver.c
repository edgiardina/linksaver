// Windows screensaver shell. A .scr is an ordinary exe that Windows launches with:
//   /s          run fullscreen until there's user input
//   /p <hwnd>   render into the small preview in the Screen Saver Settings dialog
//   /c[:hwnd]   show settings (also used when the .scr is opened with no args)
// Arguments may use '-' instead of '/', any case, and ':' before the hwnd.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "screensaver.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#define strcasecmp _stricmp
#else
#include <strings.h>
#endif

ScreensaverMode g_scr_mode;

enum {
  kMaxBlankWindows = 16,
  // Windows often delivers a stray mouse move right as the saver starts.
  kIgnoreInputMs = 750,
  kMouseMoveThreshold = 12,
};

static SDL_Window *g_blank_windows[kMaxBlankWindows];
static int ParseArgsInner(int argc, char **argv);
static int g_num_blank_windows;
static uint32_t g_start_ticks;

#ifdef _WIN32
static HWND g_parent_hwnd;

static bool ExePathIsScr() {
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
  return n >= 4 && n < sizeof(path) && strcasecmp(path + n - 4, ".scr") == 0;
}
#endif

#ifdef _WIN32
// Without this, Windows scales our coordinates at non-100% display scaling:
// the Settings preview gets cropped and fullscreen is rendered at a lower
// resolution and stretched. Must run before any window is created.
static void BecomeDpiAware() {
  typedef BOOL (WINAPI *SetContextFn)(HANDLE);
  typedef BOOL (WINAPI *SetAwareFn)(void);
  HMODULE user32 = GetModuleHandleA("user32.dll");
  SetContextFn set_context = (SetContextFn)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
  if (set_context && set_context((HANDLE)(intptr_t)-4))
    return;
  SetAwareFn set_aware = (SetAwareFn)GetProcAddress(user32, "SetProcessDPIAware");
  if (set_aware)
    set_aware();
}
#endif

int Screensaver_ParseArgs(int argc, char **argv) {
  int used = ParseArgsInner(argc, argv);
#ifdef _WIN32
  if (g_scr_mode != kScrMode_None)
    BecomeDpiAware();
#endif
  return used;
}

static int ParseArgsInner(int argc, char **argv) {
  if (argc >= 1) {
    const char *a = argv[0];
    if (strcasecmp(a, "--linksaver") == 0) {
      g_scr_mode = kScrMode_Demo;
      return 1;
    }
    if ((a[0] == '/' || a[0] == '-') && a[1] != 0 && (a[2] == 0 || a[2] == ':')) {
      const char *hwnd_arg = (a[2] == ':') ? a + 3 : NULL;
      int used = 1;
      switch (tolower((unsigned char)a[1])) {
      case 's':
        g_scr_mode = kScrMode_Fullscreen;
        return used;
      case 'c':
        g_scr_mode = kScrMode_Configure;
        break;
      case 'p':
        g_scr_mode = kScrMode_Preview;
        if (!hwnd_arg && argc >= 2)
          hwnd_arg = argv[1], used = 2;
        break;
      default:
        return 0;
      }
#ifdef _WIN32
      if (hwnd_arg)
        g_parent_hwnd = (HWND)(uintptr_t)_strtoui64(hwnd_arg, NULL, 10);
#endif
      if (g_scr_mode == kScrMode_Preview && !hwnd_arg)
        g_scr_mode = kScrMode_Fullscreen;
      return used;
    }
  }
#ifdef _WIN32
  // Double-clicking a .scr or choosing "Configure" from Explorer passes nothing.
  if (ExePathIsScr())
    g_scr_mode = kScrMode_Configure;
#endif
  return 0;
}

// Windows runs screensavers with the working directory set to System32, but
// our assets and ini live next to the executable.
void Screensaver_ChdirToExe() {
#ifdef _WIN32
  char path[MAX_PATH];
  DWORD n = GetModuleFileNameA(NULL, path, sizeof(path));
  if (n == 0 || n >= sizeof(path))
    return;
  char *slash = strrchr(path, '\\');
  if (slash) {
    *slash = 0;
    _chdir(path);
  }
#endif
}

void Screensaver_ShowConfigure() {
#ifdef _WIN32
  char dir[MAX_PATH], msg[MAX_PATH + 256];
  if (!_getcwd(dir, sizeof(dir)))
    dir[0] = 0;
  snprintf(msg, sizeof(msg),
           "Linksaver settings are in the [Linksaver] section of:\n\n%s\\zelda3.ini\n\n"
           "Open it in Notepad?", dir);
  if (MessageBoxA(g_parent_hwnd, msg, "Linksaver", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK)
    return;
  char cmd[MAX_PATH + 64];
  snprintf(cmd, sizeof(cmd), "notepad.exe \"%s\\zelda3.ini\"", dir);
  STARTUPINFOA si = { sizeof(si) };
  PROCESS_INFORMATION pi;
  if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
#endif
}

void *Screensaver_CreatePreviewWindow(uint32_t sdl_flags) {
#ifdef _WIN32
  if (!IsWindow(g_parent_hwnd))
    return NULL;
  // Render into our own child window rather than taking over the dialog's.
  HINSTANCE inst = GetModuleHandleA(NULL);
  WNDCLASSA wc = { 0 };
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = inst;
  wc.lpszClassName = "LinksaverPreview";
  RegisterClassA(&wc);
  RECT rc;
  GetClientRect(g_parent_hwnd, &rc);
  HWND child = CreateWindowExA(0, wc.lpszClassName, "", WS_CHILD | WS_VISIBLE,
                               0, 0, rc.right - rc.left, rc.bottom - rc.top,
                               g_parent_hwnd, NULL, inst, NULL);
  if (!child)
    return NULL;
  return SDL_CreateWindowFrom(child);
#else
  return NULL;
#endif
}

bool Screensaver_PreviewParentAlive() {
#ifdef _WIN32
  return IsWindow(g_parent_hwnd) != 0;
#else
  return true;
#endif
}

void Screensaver_BlankOtherDisplays(int primary_display) {
  int n = SDL_GetNumVideoDisplays();
  for (int i = 0; i < n && g_num_blank_windows < kMaxBlankWindows; i++) {
    if (i == primary_display)
      continue;
    SDL_Rect b;
    if (SDL_GetDisplayBounds(i, &b) != 0)
      continue;
    SDL_Window *w = SDL_CreateWindow("", b.x, b.y, b.w, b.h,
                                     SDL_WINDOW_BORDERLESS | SDL_WINDOW_SKIP_TASKBAR);
    if (!w)
      continue;
    SDL_Surface *s = SDL_GetWindowSurface(w);
    if (s) {
      SDL_FillRect(s, NULL, SDL_MapRGB(s->format, 0, 0, 0));
      SDL_UpdateWindowSurface(w);
    }
    g_blank_windows[g_num_blank_windows++] = w;
  }
}

void Screensaver_DestroyBlankWindows() {
  for (int i = 0; i < g_num_blank_windows; i++)
    SDL_DestroyWindow(g_blank_windows[i]);
  g_num_blank_windows = 0;
}

bool Screensaver_ShouldExitOnEvent(const void *event) {
  if (g_scr_mode != kScrMode_Fullscreen)
    return false;
  const SDL_Event *e = (const SDL_Event *)event;
  if (g_start_ticks == 0)
    g_start_ticks = SDL_GetTicks();
  bool settled = SDL_GetTicks() - g_start_ticks >= kIgnoreInputMs;
  switch (e->type) {
  case SDL_KEYDOWN:
  case SDL_MOUSEBUTTONDOWN:
  case SDL_MOUSEWHEEL:
  case SDL_CONTROLLERBUTTONDOWN:
    return settled;
  case SDL_MOUSEMOTION: {
    static bool have_origin;
    static int origin_x, origin_y;
    int x, y;
    SDL_GetGlobalMouseState(&x, &y);
    if (!have_origin || !settled) {
      have_origin = true;
      origin_x = x, origin_y = y;
      return false;
    }
    return abs(x - origin_x) + abs(y - origin_y) > kMouseMoveThreshold;
  }
  }
  return false;
}
