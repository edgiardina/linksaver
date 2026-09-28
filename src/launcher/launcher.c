// Single-file Linksaver launcher.
//
// Built as a small GUI-subsystem exe with the real program (linksaver.exe,
// SDL2.dll, assets, reference saves, default ini) appended after it by
// tools/pack_linksaver.py. On launch it unpacks that payload once into
// %LOCALAPPDATA%\Linksaver\<hash>\ and runs linksaver.exe from there with the
// same screensaver arguments. Settings live in %APPDATA%\Linksaver\linksaver.ini
// so they survive the .scr being moved or updated.
//
// Payload layout (little endian), appended to the end of this exe:
//   repeated: u16 name_len, name bytes, u32 size, data
//   footer:   u32 payload_offset, u32 file_count, u32 hash, "LSPAYLD1"
#include <windows.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "settings_dialog.h"

enum {
  kFooterSize = 20,
  kMaxFiles = 64,
};

static const char kMagic[8] = { 'L', 'S', 'P', 'A', 'Y', 'L', 'D', '1' };

static void Fail(const char *msg) {
  MessageBoxA(NULL, msg, "Linksaver", MB_OK | MB_ICONERROR);
  ExitProcess(1);
}

static unsigned char *ReadSelf(DWORD *size_out) {
  char path[MAX_PATH];
  if (!GetModuleFileNameA(NULL, path, sizeof(path)))
    return NULL;
  HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return NULL;
  DWORD size = GetFileSize(h, NULL), got = 0;
  unsigned char *data = malloc(size);
  if (!data || !ReadFile(h, data, size, &got, NULL) || got != size) {
    CloseHandle(h);
    free(data);
    return NULL;
  }
  CloseHandle(h);
  *size_out = size;
  return data;
}

static DWORD Read32(const unsigned char *p) {
  return p[0] | p[1] << 8 | p[2] << 16 | (DWORD)p[3] << 24;
}

// Creates every directory along |path| (which must not end in a separator).
static void MakeDirs(char *path) {
  for (char *p = path + 3; *p; p++) {
    if (*p == '\\' || *p == '/') {
      char c = *p;
      *p = 0;
      CreateDirectoryA(path, NULL);
      *p = c;
    }
  }
  CreateDirectoryA(path, NULL);
}

static bool WriteWholeFile(const char *path, const unsigned char *data, DWORD size) {
  HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
  if (h == INVALID_HANDLE_VALUE)
    return false;
  DWORD wrote = 0;
  bool ok = WriteFile(h, data, size, &wrote, NULL) && wrote == size;
  CloseHandle(h);
  return ok;
}

static bool FileExists(const char *path) {
  return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
}

// Unpacks the payload into |dir| unless a previous run already did.
static void Extract(const unsigned char *self, DWORD self_size, const char *dir,
                    const unsigned char **ini_data, DWORD *ini_size) {
  const unsigned char *footer = self + self_size - kFooterSize;
  DWORD offset = Read32(footer), count = Read32(footer + 4);
  char done_marker[MAX_PATH];
  snprintf(done_marker, sizeof(done_marker), "%s\\.complete", dir);
  bool already = FileExists(done_marker);

  const unsigned char *p = self + offset, *end = footer;
  for (DWORD i = 0; i < count; i++) {
    if (end - p < 2)
      Fail("Linksaver: payload is corrupt.");
    unsigned name_len = p[0] | p[1] << 8;
    p += 2;
    if ((DWORD)(end - p) < name_len + 4u)
      Fail("Linksaver: payload is corrupt.");
    char name[MAX_PATH];
    if (name_len >= sizeof(name))
      Fail("Linksaver: payload is corrupt.");
    memcpy(name, p, name_len);
    name[name_len] = 0;
    p += name_len;
    DWORD size = Read32(p);
    p += 4;
    if ((DWORD)(end - p) < size)
      Fail("Linksaver: payload is corrupt.");
    if (strcmp(name, "zelda3.ini") == 0) {
      *ini_data = p, *ini_size = size;
    } else if (!already) {
      char path[MAX_PATH * 2];
      snprintf(path, sizeof(path), "%s\\%s", dir, name);
      char *slash = strrchr(path, '\\');
      for (char *s = path; *s; s++)
        if (*s == '/') *s = '\\';
      slash = strrchr(path, '\\');
      *slash = 0;
      MakeDirs(path);
      *slash = '\\';
      if (!WriteWholeFile(path, p, size))
        Fail("Linksaver: couldn't unpack files to %LOCALAPPDATA%\\Linksaver.");
    }
    p += size;
  }
  if (!already)
    WriteWholeFile(done_marker, (const unsigned char *)"ok", 2);
}

// Returns a pointer to the arguments after the program name.
static const char *SkipProgramName(const char *cmd) {
  if (*cmd == '"') {
    cmd++;
    while (*cmd && *cmd != '"')
      cmd++;
    if (*cmd)
      cmd++;
  } else {
    while (*cmd && *cmd != ' ' && *cmd != '\t')
      cmd++;
  }
  while (*cmd == ' ' || *cmd == '\t')
    cmd++;
  return cmd;
}

static bool IsConfigureRequest(const char *args) {
  // "/c", "/c:1234", "-C", or nothing at all (Explorer's "Configure").
  if (*args == 0)
    return true;
  return (args[0] == '/' || args[0] == '-') && (args[1] == 'c' || args[1] == 'C') &&
         (args[2] == 0 || args[2] == ':' || args[2] == ' ');
}

// Puts the child in a job that dies with us, so when Windows ends the
// screensaver (e.g. on lock) the game process goes with it.
static HANDLE CreateKillOnCloseJob() {
  typedef HANDLE (WINAPI *CreateJobObjectAFn)(LPSECURITY_ATTRIBUTES, LPCSTR);
  typedef BOOL (WINAPI *SetInformationJobObjectFn)(HANDLE, int, LPVOID, DWORD);
  HMODULE k32 = GetModuleHandleA("kernel32.dll");
  CreateJobObjectAFn create_job = (CreateJobObjectAFn)GetProcAddress(k32, "CreateJobObjectA");
  SetInformationJobObjectFn set_info = (SetInformationJobObjectFn)GetProcAddress(k32, "SetInformationJobObject");
  if (!create_job || !set_info)
    return NULL;
  HANDLE job = create_job(NULL, NULL);
  if (!job)
    return NULL;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
  memset(&info, 0, sizeof(info));
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  // 9 = JobObjectExtendedLimitInformation
  if (!set_info(job, 9, &info, sizeof(info))) {
    CloseHandle(job);
    return NULL;
  }
  return job;
}

static void AssignToJob(HANDLE job, HANDLE process) {
  typedef BOOL (WINAPI *AssignProcessToJobObjectFn)(HANDLE, HANDLE);
  AssignProcessToJobObjectFn assign = (AssignProcessToJobObjectFn)GetProcAddress(
      GetModuleHandleA("kernel32.dll"), "AssignProcessToJobObject");
  if (job && assign)
    assign(job, process);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline_unused, int show) {
  DWORD self_size = 0;
  unsigned char *self = ReadSelf(&self_size);
  if (!self || self_size < kFooterSize ||
      memcmp(self + self_size - 8, kMagic, 8) != 0)
    Fail("Linksaver: this file has no packed game data. Build it with tools/pack_linksaver.py.");
  DWORD hash = Read32(self + self_size - 12);

  char local[MAX_PATH], roaming[MAX_PATH];
  if (!GetEnvironmentVariableA("LOCALAPPDATA", local, sizeof(local)) ||
      !GetEnvironmentVariableA("APPDATA", roaming, sizeof(roaming)))
    Fail("Linksaver: couldn't find the AppData folders.");

  char cache_dir[MAX_PATH], settings_dir[MAX_PATH], ini_path[MAX_PATH];
  snprintf(cache_dir, sizeof(cache_dir), "%s\\Linksaver\\%08lx", local, (unsigned long)hash);
  snprintf(settings_dir, sizeof(settings_dir), "%s\\Linksaver", roaming);
  snprintf(ini_path, sizeof(ini_path), "%s\\linksaver.ini", settings_dir);
  MakeDirs(cache_dir);
  MakeDirs(settings_dir);

  const unsigned char *ini_data = NULL;
  DWORD ini_size = 0;
  Extract(self, self_size, cache_dir, &ini_data, &ini_size);
  // Seed the settings file on first run; after that it's the user's.
  if (!FileExists(ini_path) && ini_data)
    WriteWholeFile(ini_path, ini_data, ini_size);
  free(self);

  const char *args = SkipProgramName(GetCommandLineA());
  if (IsConfigureRequest(args)) {
    // "/c:<hwnd>" names the Screen Saver Settings dialog as our owner.
    const char *colon = strchr(args, ':');
    HWND owner = colon ? (HWND)(uintptr_t)_strtoui64(colon + 1, NULL, 10) : NULL;
    ShowSettingsDialog(IsWindow(owner) ? owner : NULL, ini_path);
    return 0;
  }

  char cmd[MAX_PATH * 3 + 512];
  snprintf(cmd, sizeof(cmd), "\"%s\\linksaver.exe\" %s --config \"%s\"", cache_dir, args, ini_path);
  STARTUPINFOA si = { sizeof(si) };
  PROCESS_INFORMATION pi;
  HANDLE job = CreateKillOnCloseJob();
  // CREATE_NO_WINDOW: linksaver.exe is a console app; don't flash a console.
  if (!CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                      NULL, cache_dir, &si, &pi))
    Fail("Linksaver: couldn't start the game.");
  AssignToJob(job, pi.hProcess);
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hProcess);
  if (job)
    CloseHandle(job);
  return (int)code;
}
