// The launcher's cheat list: the cheats of DuckStation's cheat database for
// Fear Effect (cheats/SLUS-00920.cht next to the program, downloaded from
// github.com/duckstation/chtdb when it is missing), which ones are switched
// on (cheats/enabled.txt, one name per line), and the window to choose them.
#pragma once

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace app {

std::wstring cheatFilePath();                   // cheats/SLUS-00920.cht next to the program
std::vector<std::string> enabledCheats();       // names switched on, that are in the cheat file
// Modal cheat list over `owner`; offers to download the cheat file first.
// Returns true when the player pressed OK.
bool runCheatDialog(HWND owner);

}  // namespace app
