#include "hotkeys.h"
#include "log.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The key names and their parsing are Character Creator's and Seasons of
// Pywel's (hotkeys.cpp there).

struct Hotkey
{
    int key;            // 0 = none
    bool ctrl, shift, alt;
    wchar_t name[48];
};

// F6 - F8 are Character Creator's, F5 Seasons of Pywel's. Recruiting and
// dismissing are talk options, not keys.
static Hotkey g_keys[KEY_COUNT] = {
    { VK_F10, false, false, false, L"F10" },
};
static const char* INI_NAMES[KEY_COUNT] = { "BringBack" };
static const char* DESCRIPTIONS[KEY_COUNT] = { "Bring back" };

struct NamedKey
{
    const char* name;
    int key;
};

static const NamedKey NAMED_KEYS[] = {
    { "Insert", VK_INSERT }, { "Delete", VK_DELETE }, { "Home", VK_HOME }, { "End", VK_END },
    { "PageUp", VK_PRIOR }, { "PageDown", VK_NEXT }, { "Pause", VK_PAUSE }, { "ScrollLock", VK_SCROLL },
    { "Backspace", VK_BACK }, { "Tab", VK_TAB }, { "CapsLock", VK_CAPITAL }, { "Space", VK_SPACE },
    { "Up", VK_UP }, { "Down", VK_DOWN }, { "Left", VK_LEFT }, { "Right", VK_RIGHT },
    { "NumpadAdd", VK_ADD }, { "NumpadSubtract", VK_SUBTRACT }, { "NumpadMultiply", VK_MULTIPLY },
    { "NumpadDivide", VK_DIVIDE }, { "NumpadDecimal", VK_DECIMAL },
    { "Mouse3", VK_MBUTTON }, { "Mouse4", VK_XBUTTON1 }, { "Mouse5", VK_XBUTTON2 },
    { "Minus", VK_OEM_MINUS }, { "Plus", VK_OEM_PLUS }, { "Comma", VK_OEM_COMMA }, { "Period", VK_OEM_PERIOD },
    { "Tilde", VK_OEM_3 },
};

static int KeyOf(const char* name)
{
    size_t len = strlen(name);

    if (len == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
        return name[0];

    if (len == 1 && name[0] >= 'a' && name[0] <= 'z')
        return name[0] - 'a' + 'A';

    int n = 0;

    if ((name[0] == 'F' || name[0] == 'f') && sscanf_s(name + 1, "%d", &n) == 1 && n >= 1 && n <= 24)
        return VK_F1 + n - 1;

    if (_strnicmp(name, "Numpad", 6) == 0 && len == 7 && name[6] >= '0' && name[6] <= '9')
        return VK_NUMPAD0 + name[6] - '0';

    for (const NamedKey& k : NAMED_KEYS)
        if (_stricmp(name, k.name) == 0)
            return k.key;

    return 0;
}

static void Trim(char* s)
{
    char* start = s;

    while (*start == ' ' || *start == '\t')
        ++start;

    memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);

    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n'))
        s[--len] = 0;
}

// "Ctrl+Shift+F9" -> the key and its modifiers. False for an unknown key.
static bool Parse(const char* text, Hotkey* out)
{
    Hotkey key = {};
    char copy[64];
    strcpy_s(copy, text);
    Trim(copy);

    if (!copy[0] || _stricmp(copy, "NONE") == 0)
    {
        *out = key;
        return true;
    }

    char* context = NULL;

    for (char* part = strtok_s(copy, "+", &context); part; part = strtok_s(NULL, "+", &context))
    {
        Trim(part);

        if (_stricmp(part, "Ctrl") == 0 || _stricmp(part, "Control") == 0)
            key.ctrl = true;
        else if (_stricmp(part, "Shift") == 0)
            key.shift = true;
        else if (_stricmp(part, "Alt") == 0)
            key.alt = true;
        else if (key.key || !(key.key = KeyOf(part)))
            return false;
    }

    if (!key.key)
        return false;

    char name[64];
    strcpy_s(name, text);
    Trim(name);
    MultiByteToWideChar(CP_UTF8, 0, name, -1, key.name, _countof(key.name));
    *out = key;
    return true;
}

static const char DEFAULT_INI[] =
    "; Greymane Companions - hotkeys\r\n"
    ";\r\n"
    "; BringBack: your companions come to you.\r\n"
    "; (Their health on screen: the Party HUD mod.)\r\n"
    ";\r\n"
    "; Key names: F1-F24, A-Z, 0-9, Numpad0-Numpad9, Insert, Delete, Home, End,\r\n"
    "; PageUp, PageDown, Pause, ScrollLock, Mouse4, Mouse5\r\n"
    "; Add Ctrl+, Shift+ or Alt+ in front for a combination, e.g. Ctrl+F10.\r\n"
    "; None turns a key off. Changes apply the next time the game starts.\r\n"
    "\r\n"
    "[Hotkeys]\r\n"
    "BringBack = F10\r\n";

static const char OPTIONS_INI[] =
    "\r\n"
    "; Options (1 on, 0 off). Changes apply the next time the game starts.\r\n"
    "; TeleportEffect: a companion left far behind catches up with her own\r\n"
    ";   teleport (the green leaves, as Damiane's); if she is not back within\r\n"
    ";   5 s she is moved to you.\r\n"
    "; HideRoutine: a recruited Greymane drops what she was doing in camp and\r\n"
    ";   follows you at once.\r\n"
    "; StayBehind: a companion who was summoned to you (after a fast travel)\r\n"
    ";   stays where he is when dismissed and leaves when you are out of sight.\r\n"
    "; MapIcon: companions are shown on the map and minimap.\r\n"
    ";   MapIconStyle: 0 the party icon, 1 the character icon with the name.\r\n"
    "; Horses: when you ride, your companions ride with you (a free horse near\r\n"
    ";   them, or with HorseSpawn one made for them, kept while they follow).\r\n"
    "; DetailedLog: 1 writes much more to GreymaneCompanions.log (for reports).\r\n"
    "[Options]\r\n"
    "TeleportEffect = 1\r\n"
    "HideRoutine = 1\r\n"
    "StayBehind = 1\r\n"
    "MapIcon = 1\r\n"
    "MapIconStyle = 0\r\n"
    "Horses = 1\r\n"
    "HorseSpawn = 1\r\n"
    "DetailedLog = 0\r\n";

// The options an ini from an earlier version may lack, with their defaults.
static const char* const OPTIONS[][2] = {
    { "TeleportEffect", "1" }, { "HideRoutine", "1" }, { "StayBehind", "1" }, { "MapIcon", "1" }, { "MapIconStyle", "0" },
    { "Horses", "1" }, { "HorseSpawn", "1" }, { "DetailedLog", "0" },
};

static char g_ini[MAX_PATH] = "";

static bool Has(const char* section, const char* key, const char* path, char* value = NULL, int size = 0)
{
    char buffer[32];
    GetPrivateProfileStringA(section, key, "\x01", value ? value : buffer, value ? size : sizeof(buffer), path);
    return (value ? value : buffer)[0] != '\x01';
}

// DMM installs the plugin into bin64: the ini is written there on the first
// start, where DMM's ASI config editor finds it. One from a test version
// ([Experiments]) becomes [Options], its values kept.
static void WriteDefault(const char* path)
{
    bool have = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;

    if (have && Has("Options", "TeleportEffect", path))
    {
        for (const auto& k : OPTIONS)
            if (!Has("Options", k[0], path))
                WritePrivateProfileStringA("Options", k[0], k[1], path);

        return;
    }

    char old[16][2][32] = {};
    int kept = 0;

    if (have && Has("Experiments", "TeleportEffect", path))
    {
        static const char* const CARRIED[] = { "TeleportEffect", "HideRoutine", "DropRoutine", "StayBehind", "MapIcon", "MapIconStyle",
                                               "Horses", "HorseSpawn" };

        for (const char* key : CARRIED)
            if (Has("Experiments", key, path, old[kept][1], sizeof(old[kept][1])))
                strcpy_s(old[kept++][0], key);

        WritePrivateProfileStringA("Experiments", NULL, NULL, path);
        WritePrivateProfileStringA("HealthWindow", NULL, NULL, path);
        WritePrivateProfileStringA("Hotkeys", "HealthWindow", NULL, path);
    }

    FILE* f = NULL;
    fopen_s(&f, path, have ? "ab" : "wb");

    if (!f)
        return;

    if (!have)
        fwrite(DEFAULT_INI, 1, sizeof(DEFAULT_INI) - 1, f);

    fwrite(OPTIONS_INI, 1, sizeof(OPTIONS_INI) - 1, f);
    fclose(f);

    for (int i = 0; i < kept; ++i)
        WritePrivateProfileStringA("Options", old[i][0], old[i][1], path);

    Log("hotkeys: %s %s", have ? "added the options to" : "wrote the default", path);
}

int IniInt(const char* section, const char* key, int fallback)
{
    return g_ini[0] ? (int)GetPrivateProfileIntA(section, key, fallback, g_ini) : fallback;
}

int IniOption(const char* key, int fallback)
{
    if (!g_ini[0])
        return fallback;

    char value[32];
    return Has("Options", key, g_ini, value, sizeof(value)) ? atoi(value) : fallback;
}

void HotkeysLoad(const char* bin64)
{
    char path[MAX_PATH];
    sprintf_s(path, "%s\\GreymaneCompanions.ini", bin64);
    WriteDefault(path);
    strcpy_s(g_ini, path);

    for (int i = 0; i < KEY_COUNT; ++i)
    {
        char text[64] = "";
        GetPrivateProfileStringA("Hotkeys", INI_NAMES[i], "\x01", text, sizeof(text), path);

        if (text[0] != '\x01')
        {
            Hotkey key;

            if (Parse(text, &key))
                g_keys[i] = key;
            else
                Log("hotkeys: unknown key \"%s\" for %s - keeping %S", text, DESCRIPTIONS[i], g_keys[i].name);
        }

        Log("hotkeys: %s on %S", DESCRIPTIONS[i], g_keys[i].key ? g_keys[i].name : L"(none)");
    }
}

static bool Held(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

bool HotkeyDown(Key k)
{
    const Hotkey& key = g_keys[k];

    // Modifiers must match exactly, so Ctrl+F10 and F10 can be different keys.
    return key.key && Held(key.key) && Held(VK_CONTROL) == key.ctrl && Held(VK_SHIFT) == key.shift && Held(VK_MENU) == key.alt;
}

const wchar_t* HotkeyName(Key k) { return g_keys[k].name; }
