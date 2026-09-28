#ifndef LINKSAVER_LAUNCHER_SETTINGS_DIALOG_H_
#define LINKSAVER_LAUNCHER_SETTINGS_DIALOG_H_

#include <windows.h>

// Shows the modal Settings dialog, editing the [Linksaver] section of |ini_path|.
void ShowSettingsDialog(HWND owner, const char *ini_path);

#endif  // LINKSAVER_LAUNCHER_SETTINGS_DIALOG_H_
