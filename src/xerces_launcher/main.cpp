// Xerces.exe, the launcher role (docs/DESIGN.md 3.1, 7): the RmlUi launcher
// that finds the game, reads the mods, resolves the plan and starts the game
// role. Until the rename (7.7) folds both roles into one binary, the game
// role is the client next to it (boiii.exe -launch ...).
//
//   Xerces.exe [--home <dir>] [--view <screen>] [--theme dark|light|contrast]
//              [--language <lang>|pseudo] [--scale 1.5] [--size 1280x800]
//              [--dry-launch] [--skip-boot]
//              [--script <steps.txt>] [--seconds <n>]
//
// --dry-launch (or XERCES_DRY_LAUNCH=1) builds and logs the game's command
// line instead of starting it. --script plays timed input steps for checks:
//   +500 key down        +300 pad a         +200 view mods
//   +100 click launch-vanilla                +100 type hello
//   +100 shot C:\path\screen.png             +100 quit

#include <RmlUi_Include_Windows.h>

#include <objbase.h>
#include <shellapi.h>

#include "app.hpp"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  xerces_launcher::options opts = xerces_launcher::parse_options(argc, argv);
  LocalFree(argv);
  // The folder and file pickers are COM objects on this (UI) thread.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  int code = 0;
  {
    xerces_launcher::app launcher(std::move(opts));
    code = launcher.run();
  }
  if (SUCCEEDED(com)) {
    CoUninitialize();
  }
  return code;
}
