// The screensaver's Settings dialog: a plain Win32 window that reads and
// writes the [Linksaver] section of linksaver.ini. Built from code rather than
// a resource script since TCC has no resource compiler.
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings_dialog.h"

enum {
  kNumStarts = 13,
  kStartsInFirstColumn = 7,

  kId_Start0 = 100,
  kId_Audio = 200,
  kId_Enemies,
  kId_Widescreen,
  kId_Debug,
  kId_LightWorld,
  kId_AmbientLife,
  kId_ResetMinutes,
  kId_Advanced,
};

// Start points as written in StartSaves: the game's Light World spawn points
// by name, reference saves by chapter number. The ones in parentheses start
// Link somewhere he can't get far from yet.
typedef struct StartPoint {
  const char *token;
  const char *label;
} StartPoint;

static const StartPoint kStarts[kNumStarts] = {
  { "house", "Light World: Link's House" },
  { "sanctuary", "Light World: Sanctuary" },
  { "2", "Light World: Eastern Palace" },
  { "5", "Dark World: Pyramid" },
  { "6", "Dark World: Palace of Darkness" },
  { "7", "Dark World: Swamp Palace" },
  { "9", "Dark World: Village of Outcasts" },
  { "3", "Desert Palace (boxed in by rocks)" },
  { "4", "Death Mountain (limited)" },
  { "8", "Skull Woods (enclosed)" },
  { "10", "Ice Palace island (enclosed)" },
  { "11", "Misery Mire (limited)" },
  { "12", "Turtle Rock (limited)" },
};

static const char kDefaultStarts[] = "house, sanctuary, 2, 5, 6, 7, 9";

static const char kSection[] = "Linksaver";

static struct {
  const char *ini_path;
  HWND wnd;
  HFONT font;
  int dpi;
  bool done;
} g_dlg;

static int Scale(int v) {
  return MulDiv(v, g_dlg.dpi, 96);
}

static HWND AddControl(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id) {
  HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                           Scale(x), Scale(y), Scale(w), Scale(h),
                           g_dlg.wnd, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
  SendMessageA(c, WM_SETFONT, (WPARAM)g_dlg.font, TRUE);
  return c;
}

static bool ReadBool(const char *key, bool def) {
  char buf[16];
  GetPrivateProfileStringA(kSection, key, def ? "1" : "0", buf, sizeof(buf), g_dlg.ini_path);
  return buf[0] == '1' || buf[0] == 'y' || buf[0] == 'Y' || buf[0] == 't' || buf[0] == 'T' ||
         ((buf[0] == 'o' || buf[0] == 'O') && (buf[1] == 'n' || buf[1] == 'N'));
}

static void WriteBool(const char *key, bool v) {
  WritePrivateProfileStringA(kSection, key, v ? "1" : "0", g_dlg.ini_path);
}

static void SetCheck(int id, bool v) {
  SendDlgItemMessageA(g_dlg.wnd, id, BM_SETCHECK, v ? BST_CHECKED : BST_UNCHECKED, 0);
}

static bool GetCheck(int id) {
  return SendDlgItemMessageA(g_dlg.wnd, id, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

static void LoadSettings() {
  char starts[160];
  GetPrivateProfileStringA(kSection, "StartSaves", kDefaultStarts, starts, sizeof(starts), g_dlg.ini_path);
  for (char *item = strtok(starts, ", "); item; item = strtok(NULL, ", ")) {
    for (int i = 0; i < kNumStarts; i++)
      if (_stricmp(item, kStarts[i].token) == 0)
        SetCheck(kId_Start0 + i, true);
  }
  SetCheck(kId_Audio, ReadBool("Audio", false));
  SetCheck(kId_Enemies, !ReadBool("NoEnemies", true));
  SetCheck(kId_AmbientLife, ReadBool("AmbientLife", true));
  SetCheck(kId_Widescreen, ReadBool("Widescreen", true));
  SetCheck(kId_Debug, ReadBool("ShowDebug", false));
  SetCheck(kId_LightWorld, GetPrivateProfileIntA(kSection, "LightWorldPercent", 50, g_dlg.ini_path) > 0);
  char minutes[16];
  GetPrivateProfileStringA(kSection, "ResetMinutes", "20", minutes, sizeof(minutes), g_dlg.ini_path);
  SetDlgItemTextA(g_dlg.wnd, kId_ResetMinutes, minutes);
}

static bool SaveSettings() {
  char starts[160] = "";
  for (int i = 0; i < kNumStarts; i++) {
    if (GetCheck(kId_Start0 + i)) {
      if (starts[0])
        strcat(starts, ", ");
      strcat(starts, kStarts[i].token);
    }
  }
  if (!starts[0]) {
    MessageBoxA(g_dlg.wnd, "Pick at least one start point.", "Linksaver", MB_OK | MB_ICONWARNING);
    return false;
  }
  WritePrivateProfileStringA(kSection, "StartSaves", starts, g_dlg.ini_path);
  WriteBool("Audio", GetCheck(kId_Audio));
  WriteBool("NoEnemies", !GetCheck(kId_Enemies));
  WriteBool("AmbientLife", GetCheck(kId_AmbientLife));
  WriteBool("Widescreen", GetCheck(kId_Widescreen));
  WriteBool("ShowDebug", GetCheck(kId_Debug));
  WritePrivateProfileStringA(kSection, "LightWorldPercent", GetCheck(kId_LightWorld) ? "50" : "0", g_dlg.ini_path);
  char minutes[16];
  GetDlgItemTextA(g_dlg.wnd, kId_ResetMinutes, minutes, sizeof(minutes));
  snprintf(minutes, sizeof(minutes), "%d", atoi(minutes));
  WritePrivateProfileStringA(kSection, "ResetMinutes", minutes, g_dlg.ini_path);
  return true;
}

static void OpenAdvanced() {
  char cmd[MAX_PATH + 64];
  snprintf(cmd, sizeof(cmd), "notepad.exe \"%s\"", g_dlg.ini_path);
  STARTUPINFOA si = { sizeof(si) };
  PROCESS_INFORMATION pi;
  if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

static void BuildControls() {
  int y = 12;
  AddControl("BUTTON", "Start points", BS_GROUPBOX, 12, y, 436, 220, -1);
  y += 22;
  for (int i = 0; i < kNumStarts; i++) {
    int col = i < kStartsInFirstColumn ? 0 : 1;
    int row = col ? i - kStartsInFirstColumn : i;
    AddControl("BUTTON", kStarts[i].label, BS_AUTOCHECKBOX | WS_TABSTOP,
               24 + col * 212, y + row * 24, 206, 20, kId_Start0 + i);
  }
  y = 210;
  AddControl("STATIC", "Link starts at one of these at random.", SS_LEFT, 24, y - 2, 420, 16, -1);

  y = 244;
  AddControl("BUTTON", "Visit the Light World too (Link uses the Magic Mirror)", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y, 420, 20, kId_LightWorld);
  AddControl("BUTTON", "Show more of the world on wide screens", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y + 24, 420, 20, kId_Widescreen);
  AddControl("BUTTON", "Play music and sound", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y + 48, 420, 20, kId_Audio);
  AddControl("BUTTON", "Show harmless animals and townsfolk", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y + 72, 420, 20, kId_AmbientLife);
  AddControl("BUTTON", "Include enemies", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y + 96, 420, 20, kId_Enemies);
  AddControl("BUTTON", "Show the autopilot's collision map and route", BS_AUTOCHECKBOX | WS_TABSTOP, 16, y + 120, 420, 20, kId_Debug);

  y = 398;
  AddControl("STATIC", "Move to a new start point every", SS_LEFT, 16, y + 3, 190, 18, -1);
  AddControl("EDIT", "", ES_NUMBER | ES_RIGHT | WS_BORDER | WS_TABSTOP, 206, y, 44, 22, kId_ResetMinutes);
  AddControl("STATIC", "minutes (0 = never)", SS_LEFT, 256, y + 3, 180, 18, -1);

  y = 438;
  AddControl("BUTTON", "Advanced...", BS_PUSHBUTTON | WS_TABSTOP, 12, y, 96, 26, kId_Advanced);
  AddControl("BUTTON", "OK", BS_DEFPUSHBUTTON | WS_TABSTOP, 272, y, 84, 26, IDOK);
  AddControl("BUTTON", "Cancel", BS_PUSHBUTTON | WS_TABSTOP, 364, y, 84, 26, IDCANCEL);
}

static LRESULT CALLBACK WndProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
  case WM_COMMAND:
    switch (LOWORD(wp)) {
    case IDOK:
      if (SaveSettings())
        g_dlg.done = true;
      return 0;
    case IDCANCEL:
      g_dlg.done = true;
      return 0;
    case kId_Advanced:
      OpenAdvanced();
      return 0;
    }
    break;
  case WM_CLOSE:
    g_dlg.done = true;
    return 0;
  case WM_CTLCOLORSTATIC:
    // Make labels and checkboxes sit on the window background.
    SetBkMode((HDC)wp, TRANSPARENT);
    return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
  }
  return DefWindowProcA(wnd, msg, wp, lp);
}

void ShowSettingsDialog(HWND owner, const char *ini_path) {
  typedef BOOL (WINAPI *SetProcessDPIAwareFn)(void);
  SetProcessDPIAwareFn set_dpi_aware =
      (SetProcessDPIAwareFn)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDPIAware");
  if (set_dpi_aware)
    set_dpi_aware();

  memset(&g_dlg, 0, sizeof(g_dlg));
  g_dlg.ini_path = ini_path;
  HDC screen = GetDC(NULL);
  g_dlg.dpi = GetDeviceCaps(screen, LOGPIXELSY);
  ReleaseDC(NULL, screen);
  if (g_dlg.dpi <= 0)
    g_dlg.dpi = 96;
  g_dlg.font = CreateFontA(-MulDiv(9, g_dlg.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                           CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");

  HINSTANCE inst = GetModuleHandleA(NULL);
  WNDCLASSA wc = { 0 };
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursorA(NULL, (LPCSTR)IDC_ARROW);
  wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
  wc.lpszClassName = "LinksaverSettings";
  RegisterClassA(&wc);

  DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP;
  RECT rc = { 0, 0, Scale(460), Scale(476) };
  AdjustWindowRect(&rc, style, FALSE);
  int w = rc.right - rc.left, h = rc.bottom - rc.top;
  RECT work;
  SystemParametersInfoA(SPI_GETWORKAREA, 0, &work, 0);
  int x = work.left + (work.right - work.left - w) / 2;
  int y = work.top + (work.bottom - work.top - h) / 2;

  g_dlg.wnd = CreateWindowExA(WS_EX_DLGMODALFRAME, wc.lpszClassName, "Linksaver Settings", style,
                              x, y, w, h, owner, NULL, inst, NULL);
  if (!g_dlg.wnd)
    return;
  BuildControls();
  LoadSettings();
  if (owner)
    EnableWindow(owner, FALSE);
  // The first ShowWindow in a process can be overridden by how it was
  // launched (e.g. hidden); the second call is always honoured.
  ShowWindow(g_dlg.wnd, SW_SHOWNORMAL);
  ShowWindow(g_dlg.wnd, SW_SHOWNORMAL);
  SetFocus(GetDlgItem(g_dlg.wnd, IDOK));

  MSG msg;
  while (!g_dlg.done && GetMessageA(&msg, NULL, 0, 0) > 0) {
    // Enter/Escape/Tab handling like a real dialog.
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
      g_dlg.done = true;
      continue;
    }
    if (IsDialogMessageA(g_dlg.wnd, &msg))
      continue;
    TranslateMessage(&msg);
    DispatchMessageA(&msg);
  }
  if (owner)
    EnableWindow(owner, TRUE);
  DestroyWindow(g_dlg.wnd);
  DeleteObject(g_dlg.font);
}
