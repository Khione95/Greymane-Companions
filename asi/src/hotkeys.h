#pragma once

// The keys, from bin64\GreymaneCompanions.ini (written with the defaults on
// the first start, where DMM's plugin config editor finds it).
enum Key { KEY_RESCUE, KEY_COUNT };

void HotkeysLoad(const char* bin64);

// A number from the same ini (e.g. [Experiments]).
int IniInt(const char* section, const char* key, int fallback);
int IniOption(const char* key, int fallback);   // [Options]
bool HotkeyDown(Key key);
const wchar_t* HotkeyName(Key key);
