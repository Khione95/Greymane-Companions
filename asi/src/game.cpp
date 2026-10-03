#include "game.h"
#include "health.h"
#include "hotkeys.h"
#include "log.h"
#include "roster.h"
#include "toast.h"

#include <windows.h>
#include <intrin.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "MinHook.h"

// ---------------------------------------------------------------------------
// The game's mercenary clan (research/notes.md, research/plugin_plan_*.txt,
// research/experiment2_plan_*.txt). The player's comrades are records in the
// server clan; the ones that follow are in its accompany list.
// AccompanyMercenary - what quests call - only puts a comrade in that list;
// her AI starts following when it gets the Follow order's event. Recruit and
// Dismiss are talk options (tools/build_test4.py); the server's TryPreempt
// tells which one was chosen. Everything runs on the server's own thread: the
// server's message dispatch is hooked; choices, key requests and a 250 ms pump
// run there.
// ---------------------------------------------------------------------------

// The game build this was researched on (CrimsonDesert.exe).
static const uint32_t KNOWN_TIMESTAMP = 0x6AB28F00, KNOWN_IMAGE_SIZE = 0x173AB000;

static const uintptr_t ACCOMPANY = 0x2150930;        // AccompanyMercenary(clan, u32* err, VecRef* keys, VecRef* pairs)
static const uintptr_t SERVER_DISPATCH = 0x2773C20;  // once per message, on the server's worker thread
static const uintptr_t CHARACTER_BY_INDEX = 0x389570;  // CharacterInfo* (const u16* index); the record starts with its key
static const uintptr_t FIND_ACTOR = 0x2A82370;       // ServerActorManager::findActor(mgr, Scope* out, u32 actor key)
static const uintptr_t SCOPE_RELEASE = 0x1434880;    // releases a Scope findActor filled
static const uintptr_t POSITION = 0x4F8220;          // Pos* (position component, Pos* out)
static const uintptr_t SET_LOGOUT_DISTANCE = 0x28FA550;  // (actor, float): how far the clean-up lets her wander
static const uintptr_t DISBAND = 0x2BA10A0;          // DisbandByKeys(clan, u32* err, Span* keys): out of the list, still hired
static const uintptr_t AI_TELEPORT = 0x2AC5610;      // the server AI's teleport (ai, u32* err, u64, transform*)
static const uintptr_t TRY_PREEMPT = 0x2B36EF0;      // the server accepts the interaction the player chose on a target
static const uintptr_t CAN_SWITCH = 0x17EC980;       // bool (focus actor, u32 target key, u16* reason): may the player
                                                     // switch to that character (client UI and server); probed only
static const uintptr_t RESYNC = 0x2BCCC00;           // (position component, u32 err): the client running her AI snaps
                                                     // her to the server's position and restarts her AI chart

// Read from the calls in game functions (their own starts are not unique):
static const uintptr_t AI_SET_ACCOMPANY = 0x2AC9270;   // the AI chart's SetAccompanyType...
static const uintptr_t SET_ACCOMPANY_TYPE = 0x21523A0; // ...calls this (clan, u16 index, u8 type; no sync)
static const uintptr_t ACCOMPANY_TYPE_ACK = 0x297EF40; // ...and this (owner, Finder*, CharacterInfo*, u8* type): packet 0x940
static const uintptr_t STEP_MACHINE = 0x211DA30;       // an interaction's step: raises its AI event on the target with...
static const uintptr_t READ_LOCK = 0x393DF0;           // (void** out, lock); release: out->vt[0x20](out, 1)
static const uintptr_t AI_EVENT_INDEX = 0x206C220;     // u32 (u32 hash)
static const uintptr_t AI_EVENT_PREPARE = 0x17B62A0;   // (u32 key[3])
static const uintptr_t AI_EVENT_REQUEST = 0x17B54D0;   // (u8* request, const u32 key[3])
static const uintptr_t CLAN_TELEPORT = 0x2B98E20;      // the clan's "move a comrade to a safe spot" with...
static const uintptr_t FIELD_SCOPE = 0x17FA6C0;        // (owner, u8 scope[0x40]): the field, a reference held
static const uintptr_t TELEPORT_INIT = 0x2443E60;      // (u8 teleport[0x120])
static const uintptr_t SAFE_SPOT = 0x2B97DF0;          // (clan, field, u16 index, const float centre[3], u8* teleport)
static const uintptr_t TELEPORT = 0x2BCD120;           // (position component, u32* err, u8* teleport)
static const uintptr_t REMOVE_ACCOMPANY = 0x2151570;   // (clan, u32* err, Span16* indexes, u8 x4): DisbandByKeys calls it
static const uintptr_t INTERACTION_BY_INDEX = 0x3888B0;  // InteractionInfo* (const u16* index): TryPreempt calls it

// Log-only probes: why a comrade's catch-up teleport fails, and whether the
// clean-up's distance test finds her too far (AI_TELEPORT is probed too).
static const uintptr_t AI_TELEPORT_REQUEST = 0x29841B0;  // the client AI's teleport request (packet 0x82B)
static const uintptr_t AI_TELEPORT_CHECK = 0x2AC5BF0;    // (ai, u32* err, transform*, u64* sequence)
static const uintptr_t LOGOUT_DISTANCE_TEST = 0x28F9A20; // bool (actor, const float* from, ...): 1 = too far

static const uintptr_t CLIENT_ROOT = 0x6D69190;      // -> +0x30 -> +0x50 the player's own character (actor key +0x60)
static const uintptr_t SERVER_WORLD = 0x6D694E0;     // -> +0x10 the server's actor manager
static const uintptr_t SERVER_ACTORS = 0x6D69698;    // -> -> the server's actor manager (another way)
static const uintptr_t VT_SERVER_ACTORS = 0x5B25988; // .?AVServerActorManager@pa@@
static const uintptr_t VT_SERVER_CLAN = 0x5B2C6C8;   // .?AVServerMercenaryClanActorComponent@pa@@
static const uintptr_t VT_SERVER_AI = 0x5B28988;     // .?AVServerAiActorComponent@pa@@
static const uintptr_t VT_TO_TARGET_FINDER = 0x5B25438;  // .?AVToTargetSendTargetFinder@pa@@
static const uintptr_t FAR_DISTANCE = 0x6CF8AB8;     // float: the AI's catch-up teleport distance (64 m)
static const uintptr_t ERR_CLIENT_MOVE = 0x6CDD478;  // u32: eErrNoInvalidClientMovePosition
static const uintptr_t SLOT_SAFE_SPOT = VT_SERVER_CLAN + 0x180;  // the server clan's "move a comrade to a safe spot" (0x2B98E20)
// The server clan's removal notice to the client (packet 0xA2A TrocTrDisbandMercenaryAck): the client then
// runs its own removal with "disband" set, which marks its copy DISBANDED (no AI events, no movement) - he
// vanishes from view. The plugin's own Dismiss skips it and sends the list instead (vt+0x158, packet 0x9FB).
static const uintptr_t SLOT_DISBAND_ACK = VT_SERVER_CLAN + 0x178, DISBAND_ACK = 0x2B98D90;
static const uintptr_t SLOT_SEND_LIST = VT_SERVER_CLAN + 0x158, SEND_LIST = 0x2B98D10;
static const uintptr_t REMOVE_ACTOR_ACK = 0xB62C70;  // client handler of TrocTrRemoveActorAck 0x9D4 (probed only)
static const uintptr_t LOCAL_STRINGS_CLIENT = 0x6D69528, LOCAL_STRINGS_SERVER = 0x6D69530;  // -> -> +8 -> +0x1C0: the reasons
static const uintptr_t TELEPORT_MIN = 0x6CF52D8;     // float: teleports only happen this far from the player (32 m)

// The Follow and Standby orders' AI events (OnParty_Command_Follow_Offensive, OnParty_Command_Wait).
static const uint32_t EVENT_FOLLOW = 0xF5BFEE82, EVENT_WAIT = 0xEEBE079C;
// The chart's own catch-up teleport to the leader (_teleportType ToLeader ->
// CommonTeleportPackage: TELEPORT_START, the move, TELEPORT_END - the leaves).
static const uint32_t EVENT_TELEPORT_TO_LEADER = 0x51F2D258;
// OnResetNPCSchedule (RefreshNextSchedule, then CheckPoint#0): a dismissed
// camp comrade takes up her routine again.
static const uint32_t EVENT_RESET_SCHEDULE = 0x99DB94AD;
static const uint8_t TYPE_WAIT = 1;

struct ErrorName { uintptr_t rva; const char* name; };
static const ErrorName ERRORS[] = {
    { 0x6CF6EBC, "NotFoundFocusActor" }, { 0x6CF71A0, "NoInvalidCharacterInfo" },
    { 0x6CF6F50, "NoInvalidMercenaryData" }, { 0x6CF7E98, "NoEmptyCalledMercenaryList" },
    { 0x6CF6E5C, "InvalidAiOnceTimer" }, { 0x6CF6DC0, "InvalidAIState" }, { 0x6CDB978, "InvalidTrData" },
    { 0x6CDB970, "TrDeserializeFailed" }, { 0x6CF6E64, "InvalidAiActorComponent" },
    { 0x6CF7944, "LogoutDistanceFar" }, { 0x6CF7040, "LogoutByMercenaryCall" },
    { 0x6CF6938, "DisbandedMercenary" }, { 0x6CF77BC, "BeforeEnterToField" }, { 0x6CDB974, "TrSizeWrong" },
};

struct Prologue { uintptr_t rva; const char* hex; const char* name; };
static const Prologue PROLOGUES[] = {
    { ACCOMPANY, "48895C24184C894C24204889542410555657415441554156415748 8D6C24F0", "AccompanyMercenary" },
    { SERVER_DISPATCH, "48895C24104C894C242048894C2408555657415441554156415748 8D6C24E94881ECB0000000", "server dispatch" },
    { POSITION, "40534881EC800000008B81BC000000", "position" },
    { SCOPE_RELEASE, "48895C24104889742418574883EC20488BF1807910 00", "scope release" },
    { SET_LOGOUT_DISTANCE, "4053 4883EC40 C5F8297424 30 C5F828F1 488B4168", "logout distance" },
    { DISBAND, "48895C2408 48896C2410 4889742418 57 4154 4155 4156 4157 4883EC60 4D8BF8", "disband" },
    { AI_TELEPORT, "48895C2410 4889742418 57 4881EC20010000 488B41", "AI teleport" },
    { TRY_PREEMPT, "48895C2410 6644894C2420 55 56 57 4154 4155 4156 4157 488D6C24F9", "TryPreempt" },
    { RESYNC, "89542410 55 53 56 57 488DAC2408FFFFFF", "resync" },
    { CAN_SWITCH, "48895C2410 4889742418 57 4883EC20 488B01 498BF0 8BDA 488BF9 FF9078010000", "switch check" },
    { AI_TELEPORT_REQUEST, "48895C2408 55 56 57 4154 4155 4156 4157 488DAC2410FFFFFF 4881EC00020000 4D8BF0 488BFA 4C8BF9",
      "teleport request" },
    { AI_TELEPORT_CHECK, "48895C2410 55 56 57 4154 4155 4156 4157 488DAC2490FBFFFF", "teleport check" },
    { LOGOUT_DISTANCE_TEST, "488BC4 48895808 48896810 48897020 57 4881EC80000000 C5F82970E8 C5F82978D8 C57829 40C8 C5782948B8",
      "logout distance test" },
};

// Calls inside game functions, and where they must lead.
struct Call { uintptr_t site; uintptr_t target; const char* name; };
static const Call CALLS[] = {
    { ACCOMPANY + 0x335, CHARACTER_BY_INDEX, "CharacterInfo" },
    { AI_SET_ACCOMPANY + 0xFC, SET_ACCOMPANY_TYPE, "SetAccompanyType" },
    { AI_SET_ACCOMPANY + 0x154, ACCOMPANY_TYPE_ACK, "SetAccompanyType ack" },
    { STEP_MACHINE + 0x356, READ_LOCK, "read lock" },
    { STEP_MACHINE + 0x3D6, AI_EVENT_INDEX, "AI event index" },
    { STEP_MACHINE + 0x3E4, AI_EVENT_PREPARE, "AI event prepare" },
    { STEP_MACHINE + 0x3F3, AI_EVENT_REQUEST, "AI event request" },
    { CLAN_TELEPORT + 0x38, FIELD_SCOPE, "field scope" },
    { CLAN_TELEPORT + 0xEA, TELEPORT_INIT, "teleport init" },
    { CLAN_TELEPORT + 0x19F, SAFE_SPOT, "safe spot" },
    { CLAN_TELEPORT + 0x1BA, TELEPORT, "teleport" },
    { DISBAND + 0x1D3, REMOVE_ACCOMPANY, "remove from the list" },
    { TRY_PREEMPT + 0xBE, INTERACTION_BY_INDEX, "interaction by index" },
    { 0x297833E, RESYNC, "resync (a rejected client move)" },
};

static const float LEASH_M = 1.0e6f;       // her logout distance while she follows
static const float RESCUE_NEAR_M = 25.0f;  // a safe spot farther than this from the player is not used
static const int MAX_COMRADES = 6;   // Greymanes at once (Damiane and Oongka not counted)
static const ULONGLONG PUMP_MS = 250, STATUS_MS = 2000, START_MS = 500, FOLLOW_AFTER_WAIT_MS = 1000;
static const ULONGLONG RESCUE_COOLDOWN_MS = 15000, LOST_MS = 1000, RETRY_MS = 1000, GAP_MS = 1500;
static const ULONGLONG REMEMBERED_WAIT_MS = 60000;
static const ULONGLONG REFOLLOW_MS = 1000, CATCH_UP_AFTER_MS = 4000, CATCH_UP_COOLDOWN_MS = 15000, EFFECT_WAIT_MS = 5000;
static const float CATCH_UP_FACTOR = 1.5f;     // x the game's own catch-up distance (64 m)
static const ULONGLONG STUCK_MS = 15000;       // a hidden routine still holds her after this: her AI is restarted
static const float STUCK_NEAR_M = 8.0f, LEFT_ROUTINE_M = 1.5f;
static const int MAX_TRIES = 3;

struct Pos { float x, y, z; int16_t sx, sz; };
struct InlineVec32 { uint32_t* data; uint32_t count; uint32_t cap; uint32_t inl[4]; };
struct InlineVec64 { uint64_t* data; uint32_t count; uint32_t cap; uint64_t inl[4]; };
struct VecRef { void* a; void* b; };
struct Span32 { uint32_t* data; uint32_t count; uint32_t cap; };
struct Span16 { uint16_t* data; uint32_t count; uint32_t cap; };
struct Scope { uint8_t b[0x20]; };
struct Finder { uintptr_t vt; uint8_t zero; uint8_t pad[7]; uint64_t target; };

typedef uint32_t* (__fastcall* AccompanyFn)(uintptr_t clan, uint32_t* err, VecRef* keys, VecRef* pairs);
typedef uintptr_t (__fastcall* CharInfoFn)(const uint16_t* index);
typedef void* (__fastcall* FindActorFn)(uintptr_t mgr, Scope* out, uint32_t key);
typedef void (__fastcall* ScopeReleaseFn)(void* scope);
typedef Pos* (__fastcall* PositionFn)(uintptr_t component, Pos* out);
typedef uint64_t (__fastcall* DispatchFn)(void*, void*, void*, void*, uint64_t, uint64_t);
typedef void (__fastcall* SetLogoutDistanceFn)(uintptr_t actor, float distance);
typedef uint32_t* (__fastcall* DisbandFn)(uintptr_t clan, uint32_t* err, Span32* keys);
typedef uint32_t* (__fastcall* AiTeleportFn)(uintptr_t ai, uint32_t* err, uint64_t unused, const uint8_t* transform);
typedef bool (__fastcall* SetAccompanyTypeFn)(uintptr_t clan, uint16_t index, uint8_t type);
typedef void (__fastcall* AckFn)(uintptr_t owner, Finder* finder, uintptr_t info, uint8_t* type);
typedef void* (__fastcall* ReadLockFn)(uintptr_t* out, uintptr_t lock);
typedef uint32_t (__fastcall* EventIndexFn)(uint32_t hash);
typedef void (__fastcall* EventPrepareFn)(uint32_t* key);
typedef void* (__fastcall* EventRequestFn)(uint8_t* request, const uint32_t* key);
typedef void (__fastcall* FieldScopeFn)(uintptr_t owner, uint8_t* scope);
typedef void (__fastcall* TeleportInitFn)(uint8_t* teleport);
typedef void (__fastcall* SafeSpotFn)(uintptr_t clan, uintptr_t field, uint16_t index, const float* centre, uint8_t* teleport);
typedef void (__fastcall* TeleportFn)(uintptr_t component, uint32_t* err, uint8_t* teleport);
typedef uint32_t* (__fastcall* RemoveAccompanyFn)(uintptr_t clan, uint32_t* err, Span16* indexes, uint8_t disbandAndUnspawn,
                                                  uint8_t event, uint8_t sync, uint8_t a7);
typedef uintptr_t (__fastcall* InteractionByIndexFn)(const uint16_t* index);
typedef void (__fastcall* ResyncFn)(uintptr_t positionComponent, uint32_t err);
typedef void (__fastcall* SafeSpotSlotFn)(uintptr_t clan, uintptr_t actor);
struct ListRef { void* entries; uint32_t count; };
typedef void (__fastcall* DisbandAckFn)(uintptr_t clan, uint8_t a6, uint8_t a7, void* charKeys);
typedef void (__fastcall* SendListFn)(uintptr_t clan, ListRef* list);
typedef bool (__fastcall* CanSwitchFn)(uintptr_t focusActor, uint32_t targetKey, uint16_t* reason);
typedef uint64_t (__fastcall* TryPreemptFn)(uint64_t self, uint64_t result, uint64_t target, uint64_t index, uint64_t, uint64_t,
                                            uint64_t, uint64_t);
typedef uint64_t (__fastcall* Probe4Fn)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (__fastcall* Probe6Fn)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

static uintptr_t g_base = 0;
static AccompanyFn g_accompany;
static CharInfoFn g_charInfo;
static FindActorFn g_findActor;
static ScopeReleaseFn g_scopeRelease;
static PositionFn g_position;
static DispatchFn g_dispatch;
static SetLogoutDistanceFn g_setLogoutDistance;
static DisbandFn g_disband;
static AiTeleportFn g_aiTeleport;
static SetAccompanyTypeFn g_setAccompanyType;
static AckFn g_ack;
static ReadLockFn g_readLock;
static EventIndexFn g_eventIndex;
static EventPrepareFn g_eventPrepare;
static EventRequestFn g_eventRequest;
static FieldScopeFn g_fieldScope;
static TeleportInitFn g_teleportInit;
static SafeSpotFn g_safeSpot;
static TeleportFn g_teleport;
static RemoveAccompanyFn g_removeAccompany;
static InteractionByIndexFn g_interactionByIndex;
static TryPreemptFn g_tryPreempt;
static ResyncFn g_resync;
static SafeSpotSlotFn g_clanSafeSpot;
static DisbandAckFn g_disbandAck;
static Probe4Fn g_probeRemoveActor;
static CanSwitchFn g_canSwitch;
static Probe6Fn g_probeRequest;
static Probe4Fn g_probeCheck, g_probeDistance, g_probeTeleport;

static volatile LONG g_request = REQUEST_NONE;
static volatile LONG g_calls = 0;
static volatile LONG g_busy = 0;
static volatile ULONGLONG g_requestAt = 0;
static ULONGLONG g_lastPump = 0, g_lastStatus = 0, g_lastRescue = 0;
// [Experiments] in the ini
static bool g_teleportEffect = true, g_dropRoutine = false, g_hideRoutine = true, g_stayBehind = true;
static bool g_mapIcon = true, g_mapIconRefresh = false, g_horses = true, g_horseSpawn = true, g_horseMount = true;
static uintptr_t g_mapIconOffset = 0x104;
static bool g_detailedLog = false;                 // the status lines every 2 s (else every 30 s)
static const ULONGLONG QUIET_STATUS_MS = 30000;

// Events raised a little later (an actor key, the event, when).
struct Later { uint32_t actor, hash; const char* name; ULONGLONG due; };
static Later g_later[8];
static LONG g_callsAtStatus = 0;

// The comrades' actor keys, for the probes (other threads read them).
static volatile uint32_t g_tracked[MAX_COMRADES * 2];  // the comrades' actors, then the dismissed ones still watched

// HideRoutine: the actors whose AI sees no routine (read by the condition
// detours on the AI's thread; written in Track on the server thread).
static volatile LONG g_hidden[MAX_COMRADES], g_hiddenCount = 0;

// MapIcon: what the map's icon choice saw for each comrade (written by the
// detour on the map's thread, logged by the pump).
struct MapSeen { volatile LONG key; uint16_t origTex, tex; uint8_t noIcon, removed, kind, bits34; };
static MapSeen g_mapSeen[MAX_COMRADES];
static volatile LONG g_mapCalls[MAX_COMRADES];
static LONG g_mapLogged[MAX_COMRADES], g_mapCallsSeen[MAX_COMRADES];

// MapIcon research: the comrade's map component as the last icon choice saw
// it (comp+0x18 the icon shown, +0x38 the cached choice, +0x34/+0x35 bits).
struct MapDiag { volatile LONG seq; uint32_t caller; uint16_t shown, shown2, cache, cache2; uint8_t bits34, bits35, groggy, hide,
                 pending, relation, mode; int32_t age; };
static MapDiag g_mapDiag[MAX_COMRADES];
static LONG g_mapDiagSeen[MAX_COMRADES];
static uint32_t g_rideSeen = 0;   // the vehicle the character you play rides (0: on foot)
static bool g_checkProbe = false;
static __declspec(thread) uint32_t t_skipSafeSpot = 0;    // the actor the plugin's own Dismiss keeps where she is
static __declspec(thread) bool t_skipDisbandAck = false, t_disbandAckSkipped = false;
static __declspec(thread) uint32_t t_checked = 0, t_checkErr = 0;
static __declspec(thread) bool t_pluginTeleport = false;

static uint64_t R64(uintptr_t a) { return *(uint64_t*)a; }
static uint32_t R32(uintptr_t a) { return *(uint32_t*)a; }
static uint16_t R16(uintptr_t a) { return *(uint16_t*)a; }
static uint8_t R8(uintptr_t a) { return *(uint8_t*)a; }
static float RF(uintptr_t a) { return *(float*)a; }

static bool Matches(uintptr_t rva, const char* hex)
{
    uint8_t want[64];
    int n = 0;

    for (; *hex && n < 64; )
    {
        if (*hex == ' ')
        {
            ++hex;
            continue;
        }

        unsigned v = 0;
        for (int i = 0; i < 2; ++i, ++hex)
            v = v * 16 + (*hex <= '9' ? *hex - '0' : (*hex | 0x20) - 'a' + 10);

        want[n++] = (uint8_t)v;
    }

    const uint8_t* p = (const uint8_t*)(g_base + rva);

    if (memcmp(p, want, n) == 0)
        return true;

    // Hooked by another mod first (Greymane Companions and Party HUD both hook
    // the server loop): its jump over the first 5 bytes, the rest as expected.
    if (n > 5 && p[0] == 0xE9 && memcmp(p + 5, want + 5, n - 5) == 0)
    {
        Log("0x%llX is hooked by another mod already - chaining onto it", (unsigned long long)rva);
        return true;
    }

    return false;
}

static uintptr_t CallTarget(uintptr_t site)
{
    const uint8_t* call = (const uint8_t*)(g_base + site);
    return call[0] == 0xE8 ? (uintptr_t)(call + 5 + *(int32_t*)(call + 1)) : 0;
}

static const char* ErrorText(uint32_t err)
{
    if (err == 0)
        return "ok";

    for (const ErrorName& e : ERRORS)
        if (R32(g_base + e.rva) == err)
            return e.name;

    return "other";
}

static const char* NameOf(uint32_t key)
{
    for (const Greymane& g : ROSTER)
        if (g.key == key)
            return g.name;

    return NULL;
}

// Releases a read lock 0x393DF0 took (the game: out->vt[0x20](out, 1)).
static void Unlock(uintptr_t lock)
{
    typedef void (__fastcall* ReleaseFn)(uintptr_t lock, uint8_t read);
    ((ReleaseFn)R64(R64(lock) + 0x20))(lock, 1);
}

// ---------------------------------------------------------------------------
// The player, the clan and the comrades (server side)
// ---------------------------------------------------------------------------

struct Context
{
    uintptr_t mgr, player, owner, clan, control;
    uint32_t selfKey, controlKey;
    float px, py, pz;       // where the character you play is
    bool havePos;
    Scope playerScope, ownerScope, controlScope;
};

static uint32_t CharKeyOfIndex(const uint16_t* index)
{
    uintptr_t info = *index == 0xFFFF ? 0 : g_charInfo(index);
    return info ? R32(info) : 0;
}

static uint32_t CharKeyOfActor(uintptr_t actor)
{
    return CharKeyOfIndex((const uint16_t*)(R64(R64(actor + 0x68) + 0x20) + 0x30));
}

static bool WorldPos(uintptr_t actor, float* x, float* y, float* z)
{
    uintptr_t component = R64(R64(actor + 0x68) + 0x1A0);

    if (!component)
        return false;

    // Under the actor's read lock, as the game reads it (0x2B98F1E..0x2B98FA2).
    Pos p = {};
    uintptr_t lock = 0;
    g_readLock(&lock, R64(actor + 8));

    __try
    {
        g_position(component, &p);
    }
    __finally
    {
        Unlock(lock);
    }

    *x = p.x + p.sx * 1000.0f;
    *y = p.y;
    *z = p.z + p.sz * 1000.0f;
    return true;
}

static float Dist(float ax, float ay, float az, float bx, float by, float bz)
{
    return sqrtf((ax - bx) * (ax - bx) + (ay - by) * (ay - by) + (az - bz) * (az - bz));
}

static uintptr_t ServerManager()
{
    uintptr_t want = g_base + VT_SERVER_ACTORS;
    uintptr_t world = R64(g_base + SERVER_WORLD);
    uintptr_t a = world ? R64(world + 0x10) : 0;
    uintptr_t holder = R64(g_base + SERVER_ACTORS);
    uintptr_t b = holder ? R64(holder) : 0;
    bool aOk = a && R64(a) == want, bOk = b && R64(b) == want;
    static bool logged = false;

    if (!logged)
    {
        logged = true;
        Log("server actor manager: world route %p (%s), direct route %p (%s)", (void*)a, aOk ? "ok" : "no", (void*)b,
            bOk ? "ok" : "no");
    }

    return aOk ? a : bOk ? b : 0;
}

// The actor a Scope holds, if findActor got a reference on it (the game
// tests the acquired byte at +0x10, not the pointer at +8).
static uintptr_t Held(const Scope* s)
{
    return s->b[0x10] ? *(const uintptr_t*)(s->b + 8) : 0;
}

static bool OpenHeld(Context* c, bool quiet)
{
    uintptr_t user = R64(c->player + 0xA0);
    c->owner = user ? R64(user + 0xD0) : 0;

    // The clan's holder: held like the player when it is another actor (the
    // game takes a reference on it before it touches the clan, 0x17EE0AB).
    if (c->owner && c->owner != c->player)
    {
        g_findActor(c->mgr, &c->ownerScope, R32(c->owner + 0x60));

        if (Held(&c->ownerScope) != c->owner)
        {
            if (!quiet)
                Log("the clan's holder %p is not there", (void*)c->owner);

            return false;
        }
    }

    uintptr_t ownerUser = c->owner ? R64(c->owner + 0xA0) : 0;
    c->control = ownerUser ? R64(ownerUser + 0xD8) : 0;
    c->clan = c->owner ? R64(R64(c->owner + 0x68) + 0x110) : 0;

    if (!c->clan || R64(c->clan) != g_base + VT_SERVER_CLAN)
    {
        if (!quiet)
            Log("the player's server clan was not found (owner %p, clan %p)", (void*)c->owner, (void*)c->clan);

        return false;
    }

    c->controlKey = c->control ? CharKeyOfActor(c->control) : 0;

    // "You" is the character you play (Damiane after a switch), not the
    // client's own character: held for the position.
    uintptr_t at = c->player;

    if (c->control && c->control != c->player && c->control != c->owner)
    {
        g_findActor(c->mgr, &c->controlScope, R32(c->control + 0x60));

        if (Held(&c->controlScope) == c->control)
            at = c->control;
    }
    else if (c->control == c->owner)
        at = c->owner;

    c->havePos = WorldPos(at, &c->px, &c->py, &c->pz);
    return true;
}

// The player's server actor (a reference held until Close), owner, clan and
// control character. False: nothing held.
static bool Open(Context* c, bool quiet)
{
    memset(c, 0, sizeof(*c));
    uintptr_t root = R64(g_base + CLIENT_ROOT);
    uintptr_t holder = root ? R64(root + 0x30) : 0;
    uintptr_t self = holder ? R64(holder + 0x50) : 0;

    if (!self)
    {
        if (!quiet)
            Log("no player character yet");

        return false;
    }

    c->selfKey = R32(self + 0x60);
    c->mgr = ServerManager();

    if (!c->mgr)
    {
        if (!quiet)
            Log("the server's actor manager was not found");

        return false;
    }

    g_findActor(c->mgr, &c->playerScope, c->selfKey);
    c->player = Held(&c->playerScope);
    bool ok = false;

    __try
    {
        if (!c->player)
        {
            if (!quiet)
                Log("the player (actor %08X) is not on the server, or not logged in", c->selfKey);
        }
        else
            ok = OpenHeld(c, quiet);
    }
    __finally
    {
        if (!ok)
        {
            g_scopeRelease(&c->controlScope);
            g_scopeRelease(&c->ownerScope);
            g_scopeRelease(&c->playerScope);
        }
    }

    return ok;
}

static void Close(Context* c)
{
    g_scopeRelease(&c->controlScope);
    g_scopeRelease(&c->ownerScope);
    g_scopeRelease(&c->playerScope);
}

// Copies of the clan's lists, taken under the owner's read lock as the game
// reads them (GetAccompanyType 0x21524DE..0x215252F). The lock is never held
// while calling the game: AccompanyMercenary, SetAccompanyType and Disband
// take it exclusively. ReadClan again after each of them.
struct Entry { uint16_t index; uint32_t actor; uint8_t type; uint64_t merc; };
struct Record { uint16_t index; uint32_t actor, c4; uint64_t merc; };
static Entry g_list[64];
static Record g_records[512];
static uint32_t g_listCount = 0, g_recordCount = 0, g_remembered = 0;

static void ReadClan(Context* c)
{
    uintptr_t lock = 0;
    g_readLock(&lock, R64(c->owner + 8));

    __try
    {
        uintptr_t data = R64(c->clan + 0xB8);
        uint32_t count = R32(c->clan + 0xC0);
        g_listCount = 0;

        for (uint32_t i = 0; data && i < count && i < 64; ++i)
        {
            uintptr_t e = data + i * 0x18;
            Entry& d = g_list[g_listCount++];
            d.index = R16(e);
            d.actor = R32(e + 4);
            d.type = R8(e + 8);
            d.merc = R64(e + 0x10);
        }

        data = R64(c->clan + 0x18);
        count = R32(c->clan + 0x20);
        g_recordCount = 0;

        for (uint32_t i = 0; data && i < count && i < 512; ++i)
        {
            uintptr_t rec = R64(data + i * 8);

            if (!rec)
                continue;

            Record& d = g_records[g_recordCount++];
            d.index = R16(rec + 0x20);
            d.actor = R32(rec + 0x50);
            d.c4 = R32(rec + 0xC4);
            d.merc = R64(rec + 0x28);
        }

        g_remembered = R32(c->clan + 0xF0);
    }
    __finally
    {
        Unlock(lock);
    }
}

static const Entry* EntryOf(uint16_t index)
{
    for (uint32_t i = 0; i < g_listCount; ++i)
        if (g_list[i].index == index)
            return &g_list[i];

    return NULL;
}

// The owned record of a character, or NULL.
static const Record* RecordOf(uint32_t key)
{
    for (uint32_t i = 0; i < g_recordCount; ++i)
    {
        const Record& r = g_records[i];

        if (r.index != 0xFFFF && r.merc != ~0ull && CharKeyOfIndex(&r.index) == key)
            return &r;
    }

    return NULL;
}

static bool ActorAlive(Context* c, uint32_t actorKey)
{
    if (!actorKey)
        return false;

    Scope s = {};
    g_findActor(c->mgr, &s, actorKey);
    bool alive = Held(&s) != 0;
    g_scopeRelease(&s);
    return alive;
}

// Distance from the player to an actor (-1: not found).
static float DistanceTo(Context* c, uint32_t actorKey)
{
    float x, y, z;

    if (!actorKey || !c->havePos)
        return -1;

    Scope s = {};
    g_findActor(c->mgr, &s, actorKey);
    float d = -1;

    __try
    {
        uintptr_t actor = Held(&s);

        if (actor && WorldPos(actor, &x, &y, &z))
            d = Dist(x, y, z, c->px, c->py, c->pz);
    }
    __finally
    {
        g_scopeRelease(&s);
    }

    return d;
}

static void LogAccompanyList(const char* when)
{
    Log("accompany list %s: %u", when, g_listCount);

    for (uint32_t i = 0; i < g_listCount; ++i)
    {
        const Entry& e = g_list[i];
        uint32_t key = CharKeyOfIndex(&e.index);
        const char* name = NameOf(key);
        Log("  %s character %06X (index %u) actor %08X type %u mercenary %llX", name ? name : "-", key, e.index, e.actor, e.type,
            (unsigned long long)e.merc);
    }
}

// ---------------------------------------------------------------------------
// The game calls (server thread; none holds a lock while calling the next)
// ---------------------------------------------------------------------------

// AccompanyMercenary with {control character, comrade}: adds her to the list
// (or keeps her entry), re-arms her actor or summons her next to the leader,
// and sends the list to the client. *err is always 0 - read the entry after.
static void Register(Context* c, uint32_t key)
{
    InlineVec32 keys = {};
    keys.data = keys.inl;
    keys.count = 2;
    keys.cap = 4;
    keys.inl[0] = c->controlKey;
    keys.inl[1] = key;
    InlineVec64 pairs = {};
    pairs.data = pairs.inl;
    pairs.cap = 4;
    VecRef keyRef = { &keys, NULL }, pairRef = { &pairs, NULL };
    uint32_t err = 0xFFFFFFFF;
    g_accompany(c->clan, &err, &keyRef, &pairRef);
    Log("  AccompanyMercenary -> %08X (%s), %u new", err, ErrorText(err), pairs.count);

    for (uint32_t i = 0; i < pairs.count && i < 4; ++i)
        Log("    character %06X actor %08X", (uint32_t)pairs.data[i], (uint32_t)(pairs.data[i] >> 32));

    ReadClan(c);
}

// Her logout distance (cc+0x468); -1 if she is not found. With set, it is
// changed to distance (the game's own setter, which takes the lock).
static float LogoutDistance(Context* c, uint32_t actorKey, bool set, float distance)
{
    Scope s = {};
    g_findActor(c->mgr, &s, actorKey);
    float old = -1;

    __try
    {
        uintptr_t actor = Held(&s);

        if (actor)
        {
            old = RF(R64(R64(actor + 0x68) + 0x20) + 0x468);

            if (set)
                g_setLogoutDistance(actor, distance);
        }
    }
    __finally
    {
        g_scopeRelease(&s);
    }

    return old;
}

// 1: the server runs her AI (the game then raises an order's event on the
// server, as below); 0: a client does; -1: not found.
static int ServerRunsAi(Context* c, uint32_t actorKey, uint32_t* controller)
{
    Scope s = {};
    g_findActor(c->mgr, &s, actorKey);
    int result = -1;

    __try
    {
        uintptr_t actor = Held(&s);

        if (actor)
        {
            *controller = R32(actor + 0x90);
            uintptr_t lock = 0;
            g_readLock(&lock, R64(actor + 8));

            __try
            {
                typedef bool (__fastcall* AskFn)(uintptr_t actor, uint8_t what);
                result = ((AskFn)R64(R64(actor) + 0x188))(actor, 5) ? 1 : 0;
            }
            __finally
            {
                Unlock(lock);
            }
        }
    }
    __finally
    {
        g_scopeRelease(&s);
    }

    return result;
}

// Raises an order's AI event on her, as the game's interaction step does
// (0x211DDF9..0x211DEB9): the sender is the player. When a client runs her AI,
// the server's raise sends the event on to that client (TrocTrAiEventAck
// 0x8AA, 0x105CA588), where her AI takes it at its next tick.
static bool RaiseEvent(Context* c, uint32_t actorKey, uint32_t hash, const char* name)
{
    Scope s = {};
    g_findActor(c->mgr, &s, actorKey);
    bool ok = false;

    __try
    {
        uintptr_t actor = Held(&s);
        uintptr_t ai = actor ? R64(R64(actor + 0x68) + 0x58) : 0;

        if (!ai || R64(ai) != g_base + VT_SERVER_AI)
            Log("  %s event: no server AI on actor %08X", name, actorKey);
        else
        {
            uint32_t key[3] = { 0xFFFF, hash, 0 };
            key[2] = g_eventIndex(hash);
            g_eventPrepare(key);
            __declspec(align(16)) uint8_t request[0x100] = {};
            g_eventRequest(request, key);
            *(uint32_t*)(request + 0x18) = c->selfKey;
            *(uint32_t*)(request + 0x1C) = c->selfKey;
            *(uint32_t*)(request + 0x50) = 0;
            // The answer: bit 0 set and bit 1 clear with no error word is
            // taken - by the server's AI (1 or 2 plus flag bits, 0x181D990) or
            // sent on to her client (1, 0x105CA592); bit 1 is an error.
            uint32_t result[4] = {};
            typedef void* (__fastcall* RaiseFn)(uintptr_t ai, uint32_t* result, uint8_t* request, uint64_t zero);
            ((RaiseFn)R64(R64(ai) + 0x188))(ai, result, request, 0);
            ok = result[0] == 0 || ((result[0] & 3) == 1 && result[1] == 0);
            Log("  %s event (index %u) -> %08X %08X (%s)", name, key[2], result[0], result[1],
                ok ? (R32(actor + 0x90) ? "sent to her client" : "taken") : ErrorText(result[1]));
        }
    }
    __finally
    {
        g_scopeRelease(&s);
    }

    ReadClan(c);
    return ok;
}

// Sets her entry's type and tells the client, as the AI chart's
// SetAccompanyType does (0x2AC936C..0x2AC93C4).
static bool SetType(Context* c, uint16_t index, uint8_t type)
{
    bool set = g_setAccompanyType(c->clan, index, type);
    ReadClan(c);

    if (!set)
        return false;

    uintptr_t info = g_charInfo(&index);
    Finder f = {};
    f.vt = g_base + VT_TO_TARGET_FINDER;
    typedef uint64_t (__fastcall* TargetFn)(uintptr_t actor);
    f.target = ((TargetFn)R64(R64(c->owner) + 0xE8))(c->owner);
    g_ack(c->owner, &f, info, &type);
    return true;
}

static void Disband(Context* c, uint32_t key)
{
    uint32_t keys[1] = { key };
    Span32 span = { keys, 1, 1 };
    uint32_t err = 0xFFFFFFFF;
    g_disband(c->clan, &err, &span);
    Log("  DisbandByKeys -> %08X (%s)", err, ErrorText(err));
    ReadClan(c);
}

// Moves her to a safe spot next to the player, as the clan's own teleport
// (0x2B98E20) does around her own position. A spot farther than 25 m from the
// player is not used: then the AI's plain teleport puts her 1.5 m away.
static void TeleportHeld(Context* c, uintptr_t merc, uint32_t key, uint8_t* scope)
{
    if (!scope[0x10])
    {
        Log("  rescue: the field was not found");
        return;
    }

    uintptr_t field = *(uintptr_t*)(scope + 8);
    typedef uintptr_t (__fastcall* StageFn)(uintptr_t field);
    uintptr_t stage = ((StageFn)R64(R64(field) + 0x40))(field);
    bool excluded = false;

    if (stage)
    {
        uintptr_t lock = 0;
        g_readLock(&lock, R64(stage + 8));

        __try
        {
            uintptr_t list = R64(stage + 0x1A0);
            uint32_t count = R32(stage + 0x1A8);

            for (uint32_t i = 0; list && i < count && i < 256; ++i)
                if (R32(list + i * 4) == key)
                    excluded = true;
        }
        __finally
        {
            Unlock(lock);
        }
    }

    if (excluded)
    {
        Log("  rescue: this stage does not let her be moved");
        return;
    }

    __declspec(align(16)) uint8_t t[0x200] = {};
    g_teleportInit(t);
    t[0xE8] = 0;
    *(uint64_t*)(t + 0xEC) = 0;
    t[0xF4] = 0;
    uint16_t index = R16(R64(R64(merc + 0x68) + 0x20) + 0x30);
    float centre[3] = { c->px, c->py, c->pz };
    g_safeSpot(c->clan, field, index, centre, t);
    float* q = (float*)t;
    float d = Dist(q[0], q[1], q[2], c->px, c->py, c->pz);
    Log("  rescue: safe spot %.1f %.1f %.1f (%.1f m from you)", q[0], q[1], q[2], d);
    uint32_t err[4] = {};

    if ((q[0] != 0 || q[1] != 0 || q[2] != 0) && d <= RESCUE_NEAR_M)
    {
        g_teleport(R64(R64(merc + 0x68) + 0x1A0), err, t);
        Log("  rescue: teleport -> %08X (%s)", err[0], ErrorText(err[0]));
        return;
    }

    uintptr_t ai = R64(R64(merc + 0x68) + 0x58);

    if (!ai || R64(ai) != g_base + VT_SERVER_AI)
    {
        Log("  rescue: no server AI for the plain teleport");
        return;
    }

    memset(t, 0, sizeof(t));
    g_teleportInit(t);
    q[0] = c->px + 1.5f;
    q[1] = c->py;
    q[2] = c->pz;
    t_pluginTeleport = true;
    g_aiTeleport(ai, err, 0, t);
    t_pluginTeleport = false;
    Log("  rescue: plain teleport to %.1f %.1f %.1f -> %08X (%s)", q[0], q[1], q[2], err[0], ErrorText(err[0]));
}

static void Teleport(Context* c, uint32_t actorKey, uint32_t key)
{
    Scope s = {};
    g_findActor(c->mgr, &s, actorKey);

    __try
    {
        uintptr_t merc = Held(&s);

        if (!merc)
        {
            Log("  rescue: actor %08X is not there", actorKey);
            return;
        }

        __declspec(align(16)) uint8_t scope[0x40] = {};
        g_fieldScope(c->owner, scope);

        __try
        {
            TeleportHeld(c, merc, key, scope);
        }
        __finally
        {
            if (scope[0x10])
            {
                uintptr_t obj = *(uintptr_t*)(scope + 0x28);
                typedef void (__fastcall* DetachFn)(uintptr_t obj);
                ((DetachFn)R64(R64(obj) + 0x148))(obj);
            }

            g_scopeRelease(scope);
        }
    }
    __finally
    {
        g_scopeRelease(&s);
    }
}

// The game's answer to a rejected client move (0x2978331..0x297833E): the
// client that runs her AI snaps her to the server's position and restarts her
// AI chart (which drops the task she is on); a server-run AI is restarted on
// the server. It takes its own locks: none may be held.
static void Resync(Context* c, uint32_t actorKey, const char* why)
{
    Scope s = {};
    g_findActor(c->mgr, &s, actorKey);

    __try
    {
        uintptr_t actor = Held(&s);
        uintptr_t position = actor ? R64(R64(actor + 0x68) + 0x1A0) : 0;

        if (position)
        {
            g_resync(position, R32(g_base + ERR_CLIENT_MOVE));
            Log("  resync (%s): actor %08X", why, actorKey);
        }
    }
    __finally
    {
        g_scopeRelease(&s);
    }
}

// ---------------------------------------------------------------------------
// The comrades the plugin looks after
// ---------------------------------------------------------------------------

enum Phase { PHASE_FREE, PHASE_START, PHASE_WAIT_SENT, PHASE_ASKED, PHASE_FOLLOWING, PHASE_STANDBY, PHASE_LOST, PHASE_REFOLLOW };
static const char* PHASES[] = { "free", "starting", "standby sent", "asked to follow", "following", "standby", "lost",
                                "follow again" };

struct Comrade
{
    uint32_t key, actor;
    uint16_t index;
    Phase phase;
    ULONGLONG at, missingSince, nextTry, waitSince;
    float oldLeash;
    uint32_t leashed;  // the actor key the leash was raised on
    int tries, eventTries;
    ULONGLONG farSince, lastCatchUp;   // left far behind since; the plugin's last catch-up
    ULONGLONG effectSince;             // her own catch-up teleport was asked for (TeleportEffect)
    bool routineDropped;               // her AI was restarted to drop her routine (DropRoutine or stuck)
    bool hide;                         // HideRoutine: her AI sees no routine while she is with you
    ULONGLONG hideSince;               // she first followed with her routine hidden
    float sx, sy, sz;                  // where she was then
    bool moved;                        // she has left that spot
    float homeX, homeY, homeZ;         // where she was recruited (log only)
    bool haveHome;
    uint32_t horse;                    // Horses: her horse (kept while she is with you, as Damiane's)
    bool horseSpawned;                 // spawned for her (logged out when she leaves), else borrowed
    ULONGLONG offSince;                // off her horse while you ride, since
    int reseats;                       // seated again while you ride
    bool kicked;                       // her AI restarted once she sat on her horse (she then rides after you)
    ULONGLONG dismountAt;              // you got off: her own AI gets off too; made to after this
    uint8_t mountState;                // HorseMount: 1 her horse called to her, 2 she walks to it and mounts
    ULONGLONG mountAt;
    bool standby;      // she was in Standby when she was lost
    bool havePos, found;
    float x, y, z;
    uint8_t type;
    uint32_t flags, flags2, recordActor;
};

static Comrade g_comrades[MAX_COMRADES];
static ULONGLONG g_lastLook = 0;

static const char* Name(const Comrade* m)
{
    const char* n = NameOf(m->key);
    return n ? n : "?";
}

static void Track()
{
    LONG hidden = 0;

    for (int i = 0; i < MAX_COMRADES; ++i)
    {
        const Comrade& m = g_comrades[i];
        uint32_t actor = m.phase != PHASE_FREE ? m.actor : 0;

        if (g_tracked[i] != actor)
        {
            g_tracked[i] = actor;
            g_mapLogged[i] = 0;
        }

        LONG k = actor && m.hide ? (LONG)actor : 0;

        if (g_hidden[i] != k)
        {
            if (k)
                Log("%s: her routine is hidden from her AI (actor %08X)", Name(&m), (uint32_t)k);
            else
                Log("actor %08X: the routine is shown to her AI again", (uint32_t)g_hidden[i]);

            InterlockedExchange(&g_hidden[i], k);
        }

        hidden += k != 0;
    }

    InterlockedExchange(&g_hiddenCount, hidden);
}

static void SetPhase(Comrade* m, Phase p, ULONGLONG now)
{
    if (m->phase != p)
        Log("%s: %s -> %s", Name(m), PHASES[m->phase], PHASES[p]);

    m->phase = p;
    m->at = now;
    Track();
}

static bool Following(uint8_t type)
{
    return type >= 2 && type <= 4;
}

static void Leash(Context* c, Comrade* m)
{
    if (!m->actor || m->leashed == m->actor)
        return;

    float old = LogoutDistance(c, m->actor, true, LEASH_M);

    if (old < 0)
        return;

    if (old > 0 && old < LEASH_M)
        m->oldLeash = old;

    m->leashed = m->actor;
    Log("%s: logout distance %.0f -> %.0f m (actor %08X)", Name(m), old, LEASH_M, m->actor);
}

static void Depart(Context* c, uint32_t key, uint32_t actor, float oldLeash, ULONGLONG now, uint16_t index, bool summoned,
                   const float* home);

// keepLeash: Dismiss - her leash stays until she is out of your sight (Depart).
// summoned: a summoned copy that stays behind (StayBehind), unsummoned then.
static void DropHorse(Context* c, Comrade* m, const char* why, bool remove = true);

static void Release(Context* c, Comrade* m, const char* why, ULONGLONG now, bool keepLeash = false, bool summoned = false)
{
    if (m->horse)
        DropHorse(c, m, why);

    if (keepLeash && (m->leashed || m->actor))
        Depart(c, m->key, summoned ? m->actor : m->leashed ? m->leashed : m->actor, summoned || !m->leashed ? 0 : m->oldLeash, now,
               m->index, summoned, m->haveHome ? &m->homeX : NULL);
    else if (m->leashed && m->oldLeash > 0 && ActorAlive(c, m->leashed))
    {
        LogoutDistance(c, m->leashed, true, m->oldLeash);
        Log("%s: logout distance back to %.0f m", Name(m), m->oldLeash);
    }

    Log("%s: no longer looked after (%s)", Name(m), why);
    SetPhase(m, PHASE_FREE, now);
    memset(m, 0, sizeof(*m));
    Track();
}

static Comrade* Find(uint32_t key)
{
    for (Comrade& m : g_comrades)
        if (m.phase != PHASE_FREE && m.key == key)
            return &m;

    return NULL;
}

static Comrade* Add(uint32_t key, uint16_t index)
{
    for (Comrade& m : g_comrades)
        if (m.phase == PHASE_FREE)
        {
            memset(&m, 0, sizeof(m));
            m.key = key;
            m.index = index;
            return &m;
        }

    return NULL;
}

// ---------------------------------------------------------------------------
// Dismissed comrades: watched for a while (log), and their leash kept until
// they are out of your sight - the clean-up measures from you (0x2C37340), so
// a logout (back home) is then unseen. A summoned copy that stays behind
// (StayBehind) is unsummoned then, as Damiane is (DisbandByKeys).
// ---------------------------------------------------------------------------

static const ULONGLONG DEPART_LOG_MS = 60000, DEPART_LOG_EVERY_MS = 2000, DEPART_LATER_LOG_MS = 30000, DEPART_MAX_MS = 600000;
static const ULONGLONG DEPART_RETRY_MS = 2000;
static const float DEPART_OUT_OF_SIGHT_M = 100.0f, DEPART_RESTORE_NEAR_M = 50.0f, TRAVEL_M = 100.0f, LOGOUT_DEFAULT_M = 64.0f;

struct Departed
{
    uint32_t key, actor;
    uint16_t index;
    bool summoned;   // a summoned copy staying behind: unsummoned out of sight
    ULONGLONG since, lastLog, nextTry;
    float oldLeash;  // 0: no logout distance to restore
    float x, y, z;
    bool havePos;
    float homeX, homeY, homeZ;
    bool haveHome;
};

static Departed g_departed[MAX_COMRADES];
static float g_lastPx, g_lastPy, g_lastPz;
static bool g_haveLastP = false;
static ULONGLONG g_lastWatch = 0;

static uint16_t HpKey();
static void ReadHealth(Context* c, uint32_t actorKey, uint16_t hpKey, Health* row);
static Comrade* Add(uint32_t key, uint16_t index);
static Comrade* Find(uint32_t key);

static void TrackDeparted()
{
    for (int i = 0; i < MAX_COMRADES; ++i)
        g_tracked[MAX_COMRADES + i] = g_departed[i].actor;
}

static bool DepartingSummoned(uint32_t key)
{
    for (const Departed& d : g_departed)
        if (d.actor && d.summoned && d.key == key)
            return true;

    return false;
}

static void Register(Context* c, uint32_t key);

// The summoned copy leaves: the game's own unsummon (DisbandByKeys), with no
// safe-spot move. It needs him on the list: Dismiss took him off it, so he is
// put back for this moment (his actor is still there: re-armed, not summoned
// again). False: still on the list.
static bool Unsummon(Context* c, Departed* d)
{
    ReadClan(c);

    if (!EntryOf(d->index))
    {
        if (g_remembered || !c->controlKey)
            return false;

        Register(c, d->key);
        const Entry* e = EntryOf(d->index);

        if (!e)
            return false;

        if (e->actor && e->actor != d->actor)
            Log("  (the game made a new actor %08X for him - it goes too)", e->actor);

        d->actor = e->actor ? e->actor : d->actor;
    }

    t_skipSafeSpot = d->actor;
    Disband(c, d->key);
    t_skipSafeSpot = 0;
    return !EntryOf(d->index);
}

static void Depart(Context* c, uint32_t key, uint32_t actor, float oldLeash, ULONGLONG now, uint16_t index, bool summoned,
                   const float* home)
{
    Departed* slot = NULL;

    for (Departed& d : g_departed)
        if (d.actor == actor)
            slot = &d;

    for (Departed& d : g_departed)
        if (!slot && !d.actor)
            slot = &d;

    // Full: one whose actor is gone, else one near you, else the oldest.
    if (!slot)
    {
        float farM = -1;

        for (Departed& d : g_departed)
            if (!slot && !ActorAlive(c, d.actor))
                slot = &d;

        for (Departed& d : g_departed)
        {
            float dist = DistanceTo(c, d.actor);

            if (!slot && dist >= 0 && dist < DEPART_RESTORE_NEAR_M)
                slot = &d, farM = dist;
        }

        if (!slot)
        {
            slot = &g_departed[0];

            for (Departed& d : g_departed)
                if (d.since < slot->since)
                    slot = &d;

            farM = DistanceTo(c, slot->actor);
        }

        const char* name = NameOf(slot->key);
        Log("dismissed %s: no longer watched (%.0f m from you)%s", name ? name : "?", farM,
            farM > LOGOUT_DEFAULT_M ? " - may vanish in view" : "");

        if (slot->summoned)
            Unsummon(c, slot);
        else if (slot->oldLeash > 0 && ActorAlive(c, slot->actor))
            LogoutDistance(c, slot->actor, true, slot->oldLeash);
    }

    memset(slot, 0, sizeof(*slot));
    slot->key = key;
    slot->actor = actor;
    slot->index = index;
    slot->summoned = summoned;
    slot->since = now;
    slot->oldLeash = oldLeash;

    if (home)
    {
        slot->homeX = home[0];
        slot->homeY = home[1];
        slot->homeZ = home[2];
        slot->haveHome = true;
    }

    TrackDeparted();
}

// Recruited again while watched: her leash is the comrade's again.
static float Undepart(uint32_t actor)
{
    for (Departed& d : g_departed)
        if (d.actor && d.actor == actor)
        {
            float old = d.oldLeash;
            memset(&d, 0, sizeof(d));
            TrackDeparted();
            return old;
        }

    return 0;
}

static void WatchDeparted(Context* c, ULONGLONG now)
{
    // A fast travel or a loading screen since the last look.
    bool travelled = (g_haveLastP && c->havePos && Dist(c->px, c->py, c->pz, g_lastPx, g_lastPy, g_lastPz) > TRAVEL_M) ||
                     (g_lastWatch && now - g_lastWatch > GAP_MS);

    for (Departed& d : g_departed)
    {
        if (!d.actor)
            continue;

        const char* name = NameOf(d.key);

        if (!name)
            name = "?";

        // Staying behind: off the list (Dismiss took him off), unsummoned
        // when you are out of sight. Back on it in Follow: called again.
        const Entry* e = NULL;

        if (d.summoned)
        {
            ReadClan(c);
            e = EntryOf(d.index);
        }

        if (e)
        {
            if (Following(e->type) && Find(d.key))
            {
                memset(&d, 0, sizeof(d));   // recruited again
                TrackDeparted();
                continue;
            }

            if (Following(e->type))
            {
                Comrade* m = Add(d.key, d.index);

                if (m)
                {
                    m->actor = e->actor;
                    m->leashed = e->actor;
                    Log("dismissed %s: Follow pressed - looked after again", name);
                    memset(&d, 0, sizeof(d));
                    TrackDeparted();
                    SetPhase(m, PHASE_FOLLOWING, now);
                }
                else
                {
                    Log("dismissed %s: Follow pressed - %d companions already, left to the game", name, MAX_COMRADES);
                    memset(&d, 0, sizeof(d));
                    TrackDeparted();
                }

                continue;
            }

            if (e->actor && e->actor != d.actor)
            {
                d.actor = e->actor;
                TrackDeparted();
            }
        }

        Scope s = {};
        g_findActor(c->mgr, &s, d.actor);
        bool found = false, havePos = false;
        float x = 0, y = 0, z = 0, dist = -1;

        __try
        {
            uintptr_t actor = Held(&s);
            found = actor != 0;
            havePos = actor && WorldPos(actor, &x, &y, &z);
            dist = havePos && c->havePos ? Dist(x, y, z, c->px, c->py, c->pz) : -1;
            ULONGLONG age = now - d.since;

            if (age < DEPART_LOG_MS ? now - d.lastLog >= DEPART_LOG_EVERY_MS : now - d.lastLog >= DEPART_LATER_LOG_MS)
            {
                uintptr_t cc = actor ? R64(R64(actor + 0x68) + 0x20) : 0;
                uintptr_t ai = actor ? R64(R64(actor + 0x68) + 0x58) : 0;
                uint32_t flags = cc ? R32(cc + 0x390) : 0, flags2 = cc ? R32(cc + 0x394) : 0;
                float moved = havePos && d.havePos ? Dist(x, y, z, d.x, d.y, d.z) : 0;
                float home = havePos && d.haveHome ? Dist(x, y, z, d.homeX, d.homeY, d.homeZ) : -1;
                d.lastLog = now;

                if (!found)
                    Log("dismissed %s (+%.1f s): actor %08X gone", name, age / 1000.0, d.actor);
                else
                    Log("dismissed %s (+%.1f s): %.1f m from you, %.1f m from her recruit spot, moved %.1f m, flags %08X/%08X%s%s,"
                        " spawn type %u, logout distance %.0f, AI sequence %llX%s", name, age / 1000.0, dist, home, moved, flags,
                        flags2, (flags | flags2) & 0x8000000 ? " HIDDEN" : "", (flags | flags2) & 0x100000 ? " DISBANDED" : "",
                        cc ? R8(cc + 0x2D0) : 0, cc ? RF(cc + 0x468) : 0, ai ? (unsigned long long)R64(ai + 0x18) : 0ull,
                        d.summoned ? " (stays behind)" : "");
            }

            if (havePos)
            {
                d.x = x;
                d.y = y;
                d.z = z;
                d.havePos = true;
            }
        }
        __finally
        {
            g_scopeRelease(&s);
        }

        bool done = false;

        if (d.summoned)
        {
            bool away = !found || dist > DEPART_OUT_OF_SIGHT_M || travelled;

            if (!found && !e)
            {
                // Gone already (a travel logged him out): nothing to unsummon -
                // putting him on the list would summon him next to you.
                const Record* rec = RecordOf(d.key);
                Log("dismissed %s: his summoned actor is gone (record actor %08X, +0xC4 %08X)", name, rec ? rec->actor : 0,
                    rec ? rec->c4 : 0);
                done = true;
            }
            else if (away && now >= d.nextTry)
            {
                Health h = {};
                ReadHealth(c, d.actor, HpKey(), &h);

                if (found && (h.state == HEALTH_DOWN || h.state == HEALTH_DEAD))
                    d.nextTry = now + DEPART_RETRY_MS;
                else
                {
                    Log("dismissed %s: %s - he leaves", name, !found ? "not loaded" : travelled ? "you travelled" : "out of your sight");

                    if (Unsummon(c, &d))
                        done = true;
                    else
                    {
                        d.nextTry = now + DEPART_RETRY_MS;
                        Log("dismissed %s: still on the list - trying again", name);
                    }
                }
            }
        }
        else if (!found)
            done = true;
        else if (dist > DEPART_OUT_OF_SIGHT_M || travelled ||
                 (now - d.since >= DEPART_MAX_MS && dist >= 0 && dist < DEPART_RESTORE_NEAR_M))
        {
            if (d.oldLeash > 0)
            {
                LogoutDistance(c, d.actor, true, d.oldLeash);
                Log("dismissed %s: %s, %.0f m from you - logout distance back to %.0f m", name,
                    travelled ? "you travelled" : dist > DEPART_OUT_OF_SIGHT_M ? "out of your sight" : "watched long enough", dist,
                    d.oldLeash);
            }

            done = true;
        }

        if (done)
        {
            memset(&d, 0, sizeof(d));
            TrackDeparted();
        }
    }

    if (c->havePos)
    {
        g_lastPx = c->px;
        g_lastPy = c->py;
        g_lastPz = c->pz;
        g_haveLastP = true;
    }

    g_lastWatch = now;
}

// A new actor for her (the game re-summoned her, or a re-registration did):
// raise her leash and start following again (or keep her in Standby).
// Her actor's spawn type (cc+0x2D0: 0xC NPCSchedule for the camp's own actor,
// 0xE SummonMercenary for one the clan summoned next to her leader); -1 if
// she is not found.
static int SpawnType(Context* c, uint32_t actorKey)
{
    Scope s = {};
    g_findActor(c->mgr, &s, actorKey);
    int type = -1;
    uintptr_t actor = Held(&s);

    if (actor)
        type = R8(R64(R64(actor + 0x68) + 0x20) + 0x2D0);

    g_scopeRelease(&s);
    return type;
}

static const int SPAWN_NPC_SCHEDULE = 0xC, SPAWN_SUMMONED = 0xE;
static bool g_condInstalled = false;   // the HideRoutine detours are in

static void Adopt(Context* c, Comrade* m, uint32_t actor, ULONGLONG now)
{
    m->actor = actor;
    m->routineDropped = false;
    m->effectSince = 0;
    m->missingSince = 0;
    m->tries = 0;
    m->eventTries = 0;
    m->waitSince = 0;
    m->hideSince = 0;
    m->moved = false;
    m->hide = g_hideRoutine && g_condInstalled && SpawnType(c, actor) == SPAWN_NPC_SCHEDULE;  // a summoned copy has none
    Track();
    Leash(c, m);

    if (m->standby || m->phase == PHASE_STANDBY)
    {
        m->standby = false;
        bool set = SetType(c, m->index, TYPE_WAIT);
        Log("%s: kept in Standby%s", Name(m), set ? "" : " (the type could not be set)");
        SetPhase(m, PHASE_STANDBY, now);
    }
    else if (SpawnType(c, actor) == SPAWN_SUMMONED)
    {
        // Summoned next to you with type 2, as the game brings Damiane back
        // after a warp: she follows as she is - asking would stop her.
        Log("%s: summoned next to you - left following", Name(m));
        SetPhase(m, PHASE_FOLLOWING, now);
    }
    else
        SetPhase(m, PHASE_START, now);
}

// Make the game show vanilla Follow on her (type Wait), for the player to
// press: what to do when a client runs her AI.
static void Ask(Context* c, Comrade* m, ULONGLONG now)
{
    bool set = SetType(c, m->index, TYPE_WAIT);
    Log("%s: type set to Standby%s - talk to her and press Follow", Name(m), set ? "" : " FAILED");
    SetPhase(m, PHASE_ASKED, now);
}

// The start of following: replay what worked by hand (Standby, then Follow),
// whoever runs her AI (the server sends the orders on to her client). If her
// AI refuses them, the player is asked to press Follow.
static void Start(Context* c, Comrade* m, ULONGLONG now)
{
    uint32_t controller = 0;
    int server = ServerRunsAi(c, m->actor, &controller);
    uintptr_t userActor = R64(c->player + 0xA0);
    uint32_t user = userActor ? R32(userActor + 0x60) : 0;
    Log("%s: AI run by %s (actor+0x90 %08X, your user %08X)", Name(m), server == 1 ? "the server" : server == 0 ? "a client" : "?",
        controller, user);

    if (server >= 0)
    {
        if (RaiseEvent(c, m->actor, EVENT_WAIT, "Standby"))
        {
            m->eventTries = 0;
            SetPhase(m, PHASE_WAIT_SENT, now);
        }
        else if (++m->eventTries >= MAX_TRIES)
        {
            Log("%s: her AI refuses the orders - asking you instead", Name(m));
            m->eventTries = 0;
            Ask(c, m, now);
        }
        else
            m->at = now + RETRY_MS - START_MS;  // try again in a second
    }
}

// Follow, after Standby. Her chart sets type 2 when it takes it; until then she
// waits as "asked" (vanilla Follow shows on her meanwhile).
static void SendFollow(Context* c, Comrade* m, ULONGLONG now)
{
    if (RaiseEvent(c, m->actor, EVENT_FOLLOW, "Follow"))
    {
        m->eventTries = 0;
        SetPhase(m, PHASE_ASKED, now);
    }
    else if (++m->eventTries >= MAX_TRIES)
    {
        Log("%s: her AI refuses Follow - asking you instead", Name(m));
        m->eventTries = 0;
        Ask(c, m, now);
    }
    else
        m->at = now + RETRY_MS - FOLLOW_AFTER_WAIT_MS;
}

static void LogStatus(Context* c, Comrade* m, bool full)
{
    Scope s = {};
    g_findActor(c->mgr, &s, m->actor);

    __try
    {
        uintptr_t actor = Held(&s);
        const Record* rec = RecordOf(m->key);
        const Entry* entry = EntryOf(m->index);
        uint32_t recordActor = rec ? rec->actor : 0, recordC4 = rec ? rec->c4 : 0;
        uint8_t type = entry ? entry->type : 0;
        uint32_t entryActor = entry ? entry->actor : 0;
        float x = 0, y = 0, z = 0;
        bool havePos = actor && WorldPos(actor, &x, &y, &z);
        float d = havePos && c->havePos ? Dist(x, y, z, c->px, c->py, c->pz) : -1;
        float jump = havePos && m->havePos ? Dist(x, y, z, m->x, m->y, m->z) : 0;
        uintptr_t cc = actor ? R64(R64(actor + 0x68) + 0x20) : 0;
        uint32_t flags = cc ? R32(cc + 0x390) : 0, flags2 = cc ? R32(cc + 0x394) : 0;
        bool changed = (actor != 0) != m->found || type != m->type || recordActor != m->recordActor || flags != m->flags ||
                       flags2 != m->flags2 || jump > 30;

        if (full || changed)
        {
            uintptr_t b = actor ? R64(R64(actor + 0x68) + 0xB0) : 0;
            uintptr_t merc = actor ? R64(R64(actor + 0x68) + 0x118) : 0;
            uintptr_t ai = actor ? R64(R64(actor + 0x68) + 0x58) : 0;
            Log("%s [%s]: actor %08X %s, record actor %08X, entry %s (actor %08X, type %u), %.1f m from you%s", Name(m),
                PHASES[m->phase], m->actor, actor ? "found" : "NOT FOUND", recordActor, entry ? "yes" : "NO", entryActor, type, d,
                jump > 30 ? " (JUMPED)" : "");

            if (cc)
                Log("  flags %08X/%08X%s%s, spawn type %u, logout distance %.0f, +0x428 %08X, +0x278 %08X, spawner %08X,"
                    " leader %08X, controller %08X, AI sequence %llX, record +0xC4 %08X",
                    flags, flags2, (flags | flags2) & 0x8000000 ? " HIDDEN" : "", (flags | flags2) & 0x100000 ? " DISBANDED" : "",
                    R8(cc + 0x2D0), RF(cc + 0x468), R32(cc + 0x428), R32(cc + 0x278), b ? R32(b + 0x2D8) : 0,
                    merc ? R32(merc + 0x1C) : 0, R32(actor + 0x90), ai ? (unsigned long long)R64(ai + 0x18) : 0ull, recordC4);
        }

        m->found = actor != 0;
        m->type = type;
        m->recordActor = recordActor;
        m->flags = flags;
        m->flags2 = flags2;

        if (havePos)
        {
            m->x = x;
            m->y = y;
            m->z = z;
            m->havePos = true;
        }
    }
    __finally
    {
        g_scopeRelease(&s);
    }
}

// Waits while the game brings back its remembered comrades itself
// (AccompanyMercenary with keys would empty that list). False: not now.
static bool RememberedClear(Comrade* m, ULONGLONG now)
{
    if (!g_remembered)
    {
        if (m->waitSince)
            Log("%s: the game's remembered comrades are back", Name(m));

        m->waitSince = 0;
        return true;
    }

    if (!m->waitSince)
    {
        m->waitSince = now;
        Log("%s: waiting - the game brings back its remembered comrades itself (%u)", Name(m), g_remembered);
    }

    return false;
}

// Re-registers a comrade whose actor is gone (the game logged her out) or
// whose entry lost its actor: AccompanyMercenary re-arms her new actor, or
// summons her next to you.
static void Recover(Context* c, Comrade* m, ULONGLONG now)
{
    if (now < m->nextTry || !c->controlKey)
        return;

    if (!RememberedClear(m, now))
    {
        if (now - m->waitSince >= REMEMBERED_WAIT_MS)
            Release(c, m, "the game kept its remembered comrades too long", now);

        return;
    }

    ++m->tries;
    Log("%s: lost - registering her again (try %d)", Name(m), m->tries);
    Register(c, m->key);
    const Entry* entry = EntryOf(m->index);
    bool listed = entry != NULL;
    uint32_t actor = entry ? entry->actor : 0;

    if (actor && ActorAlive(c, actor))
    {
        Log("%s: back on the list, actor %08X, %.1f m from you (F10 brings her to you)", Name(m), actor, DistanceTo(c, actor));
        Adopt(c, m, actor, now);
        return;
    }

    if (listed && !actor)
    {
        // An entry without an actor makes the client drop every list update.
        Log("%s: the summon failed - taking her entry out", Name(m));
        Disband(c, m->key);
    }

    if (m->tries >= MAX_TRIES)
    {
        Release(c, m, "gave up", now);
        return;
    }

    m->nextTry = now + (m->tries == 1 ? 10000 : 30000);
}

// Next to the player: a safe spot (or 1.5 m away), then the resync so the
// client running her AI shows her there too (the server's move is not sent
// to that client), then Follow again a second later.
static void BringBack(Context* c, Comrade* m, ULONGLONG now)
{
    Teleport(c, m->actor, m->key);
    Resync(c, m->actor, "brought back");
    Log("  now %.1f m from you", DistanceTo(c, m->actor));
    m->farSince = 0;
    m->effectSince = 0;
    m->lastCatchUp = now;
    SetPhase(m, PHASE_REFOLLOW, now);
}

// The game's own catch-up teleport happens at 64 m; one that never comes
// (her client hid her but sent no teleport) leaves her behind. Farther than
// 1.5 x that for 4 s: the plugin brings her.
static void HorseAfterTeleport(Context* c, Comrade* m, ULONGLONG now);

static void CatchUp(Context* c, Comrade* m, ULONGLONG now)
{
    float d = DistanceTo(c, m->actor);
    float limit = RF(g_base + FAR_DISTANCE) * CATCH_UP_FACTOR;

    if (d < 0 || !c->havePos || limit <= 0 || d < limit)
    {
        if (m->effectSince)
        {
            Log("%s: back with her own teleport (%.0f m from you) after %llu ms", Name(m), d, now - m->effectSince);
            HorseAfterTeleport(c, m, now);
        }

        m->effectSince = 0;
        m->farSince = 0;
        return;
    }

    // Her own teleport was asked for: wait for it, then move her as before.
    if (m->effectSince)
    {
        if (now - m->effectSince >= EFFECT_WAIT_MS)
        {
            Log("%s: her own teleport did not bring her (%.0f m) - moving her", Name(m), d);
            m->effectSince = 0;
            BringBack(c, m, now);
            HorseAfterTeleport(c, m, now);
        }

        return;
    }

    if (!m->farSince)
        m->farSince = now;
    else if (now - m->farSince >= CATCH_UP_AFTER_MS && now - m->lastCatchUp >= CATCH_UP_COOLDOWN_MS)
    {
        Log("%s: left behind (%.0f m for %llu s)", Name(m), d, (now - m->farSince) / 1000);
        m->lastCatchUp = now;

        if (g_teleportEffect && RaiseEvent(c, m->actor, EVENT_TELEPORT_TO_LEADER, "Teleport to leader"))
            m->effectSince = now;
        else
        {
            BringBack(c, m, now);
            HorseAfterTeleport(c, m, now);
        }
    }
}

// The events waiting for their time.
static void LaterHeld(Context* c, ULONGLONG now)
{
    for (Later& l : g_later)
        if (l.actor && now >= l.due)
        {
            RaiseEvent(c, l.actor, l.hash, l.name);
            l.actor = 0;
        }
}

static void RaiseLater(uint32_t actor, uint32_t hash, const char* name, ULONGLONG due)
{
    for (Later& l : g_later)
        if (!l.actor)
        {
            l = { actor, hash, name, due };
            return;
        }
}

static void Look(Context* c, Comrade* m, ULONGLONG now, bool status)
{
    ReadClan(c);
    const Entry* e = EntryOf(m->index);
    bool entry = e != NULL;
    uint32_t entryActor = e ? e->actor : 0;
    uint8_t type = e ? e->type : 0;

    // A new live actor in her entry: the game re-summoned her.
    if (entry && entryActor && entryActor != m->actor && ActorAlive(c, entryActor))
    {
        Log("%s: new actor %08X (was %08X)", Name(m), entryActor, m->actor);
        Adopt(c, m, entryActor, now);
        ReadClan(c);
        e = EntryOf(m->index);
        entry = e != NULL;
        entryActor = e ? e->actor : 0;
        type = e ? e->type : 0;
    }

    LogStatus(c, m, status);

    if (m->phase == PHASE_LOST)
    {
        Recover(c, m, now);
        return;
    }

    // Her entry must hold her live actor: one without (0, or a dead key)
    // counts as missing, and the re-registration repairs it.
    bool alive = ActorAlive(c, m->actor);
    bool entryOk = entry && entryActor == m->actor;

    if (alive && entryOk)
        m->missingSince = 0;
    else
    {
        // Two polls in a row (one odd read is not a loss).
        if (!m->missingSince)
            m->missingSince = now;
        else if (now - m->missingSince >= LOST_MS)
        {
            if (!entry && alive)
            {
                Release(c, m, "taken off the list while she is there - dismissed", now);
                return;
            }

            Log("%s: actor %08X %s, entry %s (actor %08X)", Name(m), m->actor, alive ? "alive" : "gone", entry ? "kept" : "gone",
                entryActor);
            m->standby = m->phase == PHASE_STANDBY;
            SetPhase(m, PHASE_LOST, now);
            m->nextTry = now;
        }

        return;
    }

    switch (m->phase)
    {
    case PHASE_START:
        if (now >= m->at + START_MS)
            Start(c, m, now);

        break;

    case PHASE_WAIT_SENT:
        if (now >= m->at + FOLLOW_AFTER_WAIT_MS)
            SendFollow(c, m, now);

        break;

    case PHASE_ASKED:
        if (Following(type))
        {
            Log("%s: type %u - following", Name(m), type);

            // DropRoutine: with her routine hidden, her AI restarted at once
            // picks following (the resync's restart) - then Follow once more.
            if (g_dropRoutine && m->hide && !m->routineDropped)
            {
                m->routineDropped = true;
                Resync(c, m->actor, "drop the routine");
                SetPhase(m, PHASE_REFOLLOW, now);
            }
            else
            {
                SetPhase(m, PHASE_FOLLOWING, now);

                if (m->hide && !m->hideSince)
                {
                    m->hideSince = now;
                    m->sx = m->x;
                    m->sy = m->y;
                    m->sz = m->z;
                    m->moved = !m->havePos;
                }
            }
        }

        break;

    case PHASE_REFOLLOW:
        if (now >= m->at + REFOLLOW_MS)
        {
            RaiseEvent(c, m->actor, EVENT_FOLLOW, "Follow");
            SetPhase(m, PHASE_ASKED, now);
        }

        break;

    case PHASE_FOLLOWING:
        // The player pressed vanilla Standby.
        if (type == TYPE_WAIT)
            SetPhase(m, PHASE_STANDBY, now);
        else
        {
            // Her routine hidden: she leaves her spot - or, still there after
            // 15 s (a bench, a scene), her AI is restarted once.
            if (m->hide && m->hideSince && !m->moved)
            {
                float from = m->havePos ? Dist(m->x, m->y, m->z, m->sx, m->sy, m->sz) : 0;

                if (from > LEFT_ROUTINE_M)
                {
                    m->moved = true;
                    Log("%s: left her routine after %.1f s", Name(m), (now - m->hideSince) / 1000.0);
                }
                else if (!m->routineDropped && now - m->hideSince >= STUCK_MS && DistanceTo(c, m->actor) > STUCK_NEAR_M)
                {
                    m->routineDropped = true;
                    Log("%s: still on her routine after %.0f s (%.1f m from you) - restarting her AI", Name(m),
                        (now - m->hideSince) / 1000.0, DistanceTo(c, m->actor));
                    Resync(c, m->actor, "still on her routine");
                    SetPhase(m, PHASE_REFOLLOW, now);
                    break;
                }
            }

            CatchUp(c, m, now);
        }

        break;

    case PHASE_STANDBY:
        // The player pressed vanilla Follow.
        if (Following(type))
            SetPhase(m, PHASE_FOLLOWING, now);

        break;

    default:
        break;
    }
}

// Research: the party list and the character played, logged whenever either
// changes (also with no Greymane following) - character switching and the
// game's own party changes show in the log.
static uint64_t g_listSeen = 0;
static uint32_t g_playingSeen = 0;

static void WatchList(Context* c)
{
    ReadClan(c);
    uint64_t sig = 1469598103934665603ull ^ g_listCount;

    for (uint32_t i = 0; i < g_listCount; ++i)
    {
        const Entry& e = g_list[i];
        sig = (sig ^ ((uint64_t)e.index << 40 | (uint64_t)e.type << 32 | e.actor)) * 1099511628211ull;
    }

    uint32_t playing = c->control ? R32(c->control + 0x60) : 0;

    if (sig == g_listSeen && playing == g_playingSeen)
        return;

    g_listSeen = sig;
    g_playingSeen = playing;
    Log("party: you play character %06X (actor %08X); remembered %u", c->controlKey, playing, g_remembered);
    LogAccompanyList("now");
}

static void LogSeen(ULONGLONG now);
static void LogSeenStatus();

// Greymanes on the party list the plugin does not look after - after loading
// a save (the list is saved, the plugin's own state is not): looked after
// again. One whose actor is there follows again (her catch-up teleport
// brings her from the camp); one without is registered again (summoned next
// to you), as a lost one is.
static void AdoptListed(Context* c, ULONGLONG now)
{
    for (uint32_t i = 0; i < g_listCount; ++i)
    {
        Entry e = g_list[i];
        uint16_t index = e.index;

        if (index == 0xFFFF)
            continue;

        uint32_t key = CharKeyOfIndex(&index);
        const char* name = NameOf(key);

        if (!name || Find(key) || DepartingSummoned(key))
            continue;

        bool departed = false;

        for (const Departed& d : g_departed)
            departed |= d.actor && d.key == key;

        if (departed)
            continue;

        Comrade* m = Add(key, e.index);

        if (!m)
        {
            Log("party: %s is on your list, but %d companions are looked after already", name, MAX_COMRADES);
            continue;
        }

        m->standby = e.type == TYPE_WAIT;

        if (e.actor && ActorAlive(c, e.actor))
        {
            Log("party: %s is on your list (actor %08X, %.0f m from you, type %u) - looked after again", name, e.actor,
                DistanceTo(c, e.actor), e.type);
            Adopt(c, m, e.actor, now);
        }
        else
        {
            Log("party: %s is on your list without her actor - registered again shortly", name);
            m->actor = e.actor;
            SetPhase(m, PHASE_LOST, now);
            m->nextTry = now + 5000;
            m->waitSince = now;
        }
    }
}

// The followers' chase formation (Formation_MercenaryChase 0xF4244) has room
// for 2 (Damiane and Oongka): a third follower takes no place and loses the
// chase - and its catch-up teleport (SetTargetFormation 0x22F0DB0 -> 0x1577140
// refuses with eErrNoTargetFormationMemberIsFull, silently). Its member list
// (info+0x18, stride 0x18; count +0x20) is made longer: 1 + 8 places.
static const uintptr_t FORMATION_INDEX = 0x38AA20;    // bool (u16* out, u32 key)
static const uintptr_t FORMATION_INFO = 0xA07E40;     // FormationInfo* (const u16* index)
static const uint32_t FORMATION_MERCENARY_CHASE = 0xF4244;
static const int FORMATION_PLACES = 9;
__declspec(align(16)) static uint8_t g_formation[FORMATION_PLACES * 0x18];
static bool g_formationDone = false;
static ULONGLONG g_formationNext = 0;

static void WidenFormation(ULONGLONG now)
{
    if (g_formationDone || now < g_formationNext)
        return;

    g_formationNext = now + 2000;

    __try
    {
        uint16_t index = 0xFFFF;
        typedef bool (__fastcall* IndexFn)(uint16_t* out, uint32_t key);
        typedef uintptr_t (__fastcall* InfoFn)(const uint16_t* index);

        if (!((IndexFn)(g_base + FORMATION_INDEX))(&index, FORMATION_MERCENARY_CHASE) || index == 0xFFFF)
            return;

        uintptr_t info = ((InfoFn)(g_base + FORMATION_INFO))(&index);

        if (!info)
            return;

        uint32_t count = R32(info + 0x20);
        uintptr_t list = R64(info + 0x18);

        if (list == (uintptr_t)g_formation)
        {
            g_formationDone = true;
            return;
        }

        if (count != 3 || !list)
        {
            Log("formation: the chase formation has %u places - left as it is", count);
            g_formationDone = true;
            return;
        }

        // The game's 3 (the leader, then +-1 m to the side 3 m behind), then
        // 6 more behind them, copied from the last one with new places.
        memcpy(g_formation, (const void*)list, 3 * 0x18);
        static const float MORE[6][3] = { { -2, 0, -5.5f }, { 2, 0, -5.5f }, { -1, 0, -8 }, { 1, 0, -8 }, { -2, 0, -10.5f }, { 2, 0, -10.5f } };

        for (int i = 0; i < 6; ++i)
        {
            uint8_t* e = g_formation + (3 + i) * 0x18;
            memcpy(e, (const void*)(list + 2 * 0x18), 0x18);
            memcpy(e, MORE[i], 12);
        }

        const float* f1 = (const float*)(list + 0x18);
        const float* f2 = (const float*)(list + 0x30);
        InterlockedExchangePointer((void**)(info + 0x18), g_formation);
        InterlockedExchange((volatile LONG*)(info + 0x20), FORMATION_PLACES);
        g_formationDone = true;
        Log("formation: the chase formation now has %d places (was 3: %.1f %.1f %.1f / %.1f %.1f %.1f)", FORMATION_PLACES, f1[0], f1[1],
            f1[2], f2[0], f2[1], f2[2]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("formation: could not be read");
        g_formationDone = true;
    }
}

static void HorsesOnRide(Context* c, bool mounted, bool riding);
static void HorsesWatch(Context* c, ULONGLONG now);

// Research (horses): the vehicle the character you play rides, logged when it
// changes ([[[actor+0x68]+0x48]+0x18]: +0x10 the vehicle's key, +0x14 the seat).
static void WatchRide(Context* c)
{
    uintptr_t you = c->control ? c->control : c->player;
    uint32_t vehicle = 0;
    int seat = -1;

    __try
    {
        uintptr_t rc = you ? R64(R64(you + 0x68) + 0x48) : 0;
        uintptr_t block = rc ? R64(rc + 0x18) : 0;

        if (block)
        {
            vehicle = R32(block + 0x10);
            seat = (int8_t)R8(block + 0x14);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return;
    }

    if (vehicle == g_rideSeen)
        return;

    if (vehicle)
        Log("ride: you got on vehicle %08X (seat %d)", vehicle, seat);
    else
        Log("ride: you got off vehicle %08X", g_rideSeen);

    bool mounted = vehicle && !g_rideSeen;
    g_rideSeen = vehicle;
    HorsesOnRide(c, mounted, vehicle != 0);
}

static void PumpHeld(Context* c, ULONGLONG now)
{
    bool any = false;
    WatchList(c);
    AdoptListed(c, now);
    LaterHeld(c, now);
    WatchDeparted(c, now);
    LogSeen(now);
    WatchRide(c);
    HorsesWatch(c, now);
    WidenFormation(now);

    for (Comrade& m : g_comrades)
        any |= m.phase != PHASE_FREE;

    // After a gap (a loading screen), a comrade missing before it is not
    // counted as lost on the first look after it.
    if (now - g_lastLook > GAP_MS)
        for (Comrade& m : g_comrades)
            m.missingSince = 0;

    g_lastLook = now;

    if (!any)
        return;

    bool status = now - g_lastStatus >= (g_detailedLog ? STATUS_MS : QUIET_STATUS_MS);

    if (status)
    {
        ReadClan(c);
        LONG calls = g_calls;
        Log("-- you at %.1f %.1f %.1f; %ld messages/s; catch-up %.0f m, teleport minimum %.0f m; list %u, remembered %u",
            c->px, c->py, c->pz, (long)((calls - g_callsAtStatus) * 1000 / (LONG)(now - g_lastStatus + 1)), RF(g_base + FAR_DISTANCE),
            RF(g_base + TELEPORT_MIN), g_listCount, g_remembered);
        g_lastStatus = now;
        g_callsAtStatus = calls;

        for (uint32_t i = 0; i < g_listCount; ++i)
            if (g_list[i].index == 0xFFFF || !g_list[i].actor)
                Log("  WARNING: list entry %u has no character or actor - the client ignores list updates", i);

        LogSeenStatus();
    }

    for (Comrade& m : g_comrades)
        if (m.phase != PHASE_FREE)
            Look(c, &m, now, status);
}

// ---------------------------------------------------------------------------
// Health (Party HUD reads it the same way to show it)
// ---------------------------------------------------------------------------

static const uintptr_t STATUS_INFO = 0x6D69530;     // the server's status table: -> -> +8 -> +0x10 -> +0xA0 the Hp key

// The Hp status key (statusinfo row 0 "Hp"; read as the game's HP-ratio code
// does, 0x36748C..0x3674AA, with 0 when anything is missing).
static uint16_t HpKey()
{
    __try
    {
        uintptr_t a = R64(g_base + STATUS_INFO);
        a = a ? R64(a) : 0;
        a = a ? R64(a + 8) : 0;
        a = a ? R64(a + 0x10) : 0;
        return a ? R16(a + 0xA0) : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

// Her health from the status component ([[actor+0x68]+0x20], the same one
// the plugin reads her flags from), under the actor's read lock as the game
// reads it (0x22DFDA9..0x22DFDE7): the Hp gauge (u16 key, s64 current +8,
// max +0x18, min +0x28, fixed point x1000), dead (+0x273), down (the
// groggy test 0x1794ED0: +0x320 > 0, alive, min > 0, current == min).
static void ReadHealth(Context* c, uint32_t actorKey, uint16_t hpKey, Health* row)
{
    memset(row, 0, sizeof(*row));
    row->state = HEALTH_AWAY;

    if (!actorKey)
        return;

    Scope s = {};
    g_findActor(c->mgr, &s, actorKey);

    __try
    {
        uintptr_t actor = Held(&s);

        if (!actor || !R64(actor + 8))
            return;

        uintptr_t lock = 0;
        g_readLock(&lock, R64(actor + 8));

        __try
        {
            uintptr_t sc = R64(R64(actor + 0x68) + 0x20);
            row->state = HEALTH_UNKNOWN;

            if (!sc)
                return;

            uintptr_t gauges = R64(sc + 0x18);
            uintptr_t array = gauges ? R64(gauges + 0x58) : 0;
            uint32_t count = gauges ? R32(gauges + 0x60) : 0;
            bool dead = R8(sc + 0x273) != 0, groggy = (int32_t)R32(sc + 0x320) > 0;

            for (uint32_t i = 0; array && i < count && i < 64; ++i)
            {
                uintptr_t e = array + i * 0x90;

                if (R16(e) != hpKey)
                    continue;

                int64_t current = (int64_t)R64(e + 8), max = (int64_t)R64(e + 0x18), min = (int64_t)R64(e + 0x28);

                if (max > 0)
                {
                    row->hp = (float)current / 1000.0f;
                    row->maxHp = (float)max / 1000.0f;
                    row->state = dead ? HEALTH_DEAD : groggy && min > 0 && current == min ? HEALTH_DOWN : HEALTH_OK;
                }

                break;
            }

            if (dead)
                row->state = HEALTH_DEAD;
        }
        __finally
        {
            Unlock(lock);
        }
    }
    __finally
    {
        g_scopeRelease(&s);
    }
}

// ---------------------------------------------------------------------------
// The talk options: Recruit and Dismiss (tools/build_test4.py makes them).
// The server's TryPreempt tells which option the player chose on whom; the
// hook only notes it, and the dispatch hook acts on it right after the same
// server message (no game call inside TryPreempt).
// ---------------------------------------------------------------------------

static const uint32_t RECRUIT_OPTION = 0xF5010, DISMISS_OPTION = 0xF5011;

struct Choice { uint32_t option, player, target; uint16_t index; ULONGLONG at; };
static const int MAX_CHOICES = 8;
static const ULONGLONG CHOICE_MS = 5000;
static Choice g_choices[MAX_CHOICES];
static volatile LONG g_choiceCount = 0;
static SRWLOCK g_choiceLock = SRWLOCK_INIT;

static void PushChoice(const Choice& ch)
{
    AcquireSRWLockExclusive(&g_choiceLock);

    if (g_choiceCount < MAX_CHOICES)
        g_choices[g_choiceCount++] = ch;

    ReleaseSRWLockExclusive(&g_choiceLock);
}

static int TakeChoices(Choice* out)
{
    AcquireSRWLockExclusive(&g_choiceLock);
    int n = g_choiceCount;
    memcpy(out, g_choices, n * sizeof(Choice));
    g_choiceCount = 0;
    ReleaseSRWLockExclusive(&g_choiceLock);
    return n;
}

static const char* OptionName(uint32_t option)
{
    return option == RECRUIT_OPTION ? "Recruit" : "Dismiss";
}

// Recruit: on the list (AccompanyMercenary), the leash, then the proven
// sequence in this same server step - Standby, then Follow (sent on to her
// client when a client runs her AI). Her type turning 2 says she follows;
// vanilla Follow shows on her until then.
static void RecruitHeld(Context* c, const Choice& ch, ULONGLONG now)
{
    uint16_t index = ch.index;
    uint32_t key = CharKeyOfIndex(&index);
    const char* name = NameOf(key);

    if (!name)
    {
        Log("recruit: character %06X is not one of the Greymanes", key);
        return;
    }

    if (!c->control || !c->controlKey)
    {
        Log("recruit: the control character was not found");
        return;
    }

    ReadClan(c);

    if (!RecordOf(key))
    {
        Log("recruit: %s is not one of your comrades (not hired)", name);
        return;
    }

    Comrade* m = Find(key);
    bool added = m == NULL;

    if (!m && !(m = Add(key, index)))
    {
        Log("recruit: already %d companions", MAX_COMRADES);
        char text[160];
        sprintf_s(text, "%d Greymanes already follow you. Dismiss one to recruit %s.", MAX_COMRADES, name);
        Toast("Your party is full", text);
        return;
    }

    const Entry* e = EntryOf(index);
    uint32_t actor = ch.target;

    if (!e || e->actor != actor)
    {
        if (!RememberedClear(m, now))
        {
            Log("recruit: not now");

            if (added)
                Release(c, m, "not registered", now);

            return;
        }

        Register(c, key);
        e = EntryOf(index);

        if (!e)
        {
            Release(c, m, "the game did not add her", now);
            return;
        }

        if (e->actor)
            actor = e->actor;
    }

    m->actor = actor;
    m->missingSince = 0;
    m->tries = 0;
    m->eventTries = 0;
    m->standby = false;

    float departedLeash = Undepart(actor);

    if (departedLeash > 0)
    {
        m->oldLeash = departedLeash;
        m->leashed = actor;
    }

    // HideRoutine: her AI no longer sees her routine from its next tick on.
    m->hide = g_hideRoutine && g_condInstalled && SpawnType(c, actor) == SPAWN_NPC_SCHEDULE;
    m->hideSince = 0;
    m->moved = false;
    m->routineDropped = false;

    {
        Scope hs = {};
        g_findActor(c->mgr, &hs, actor);
        uintptr_t a = Held(&hs);
        m->haveHome = a && WorldPos(a, &m->homeX, &m->homeY, &m->homeZ);
        g_scopeRelease(&hs);
    }

    Track();
    Leash(c, m);
    bool set = SetType(c, index, TYPE_WAIT);
    uint32_t controller = 0;
    int server = ServerRunsAi(c, actor, &controller);
    Log("recruit: %s (%06X), actor %08X, %.1f m from you; AI run by %s (%08X); type Standby%s", name, key, actor,
        DistanceTo(c, actor), server == 1 ? "the server" : server == 0 ? "a client" : "?", controller, set ? "" : " FAILED");

    // Follow now and once more a second later. (A resync here did not stop
    // her routine - its stay time is the schedule's - and could swallow the
    // first Follow.)
    if (!RaiseEvent(c, actor, EVENT_FOLLOW, "Follow"))
        Log("recruit: her AI did not take Follow - press Follow on her");

    if (added)
    {
        int count = 0;

        for (const Comrade& k : g_comrades)
            count += k.phase != PHASE_FREE || &k == m;

        char title[96], text[96];
        sprintf_s(title, "%s has joined you", name);
        sprintf_s(text, "Greymanes in your party: %d of %d", count, MAX_COMRADES);
        Toast(title, text);
    }

    SetPhase(m, PHASE_REFOLLOW, now);
}

// Off the list with no safe-spot move and no disband notice to the client:
// the client gets the new list instead, so its copy stays as it is (not
// DISBANDED) and walks on. False: still on the list.
static bool RemoveQuietly(Context* c, uint16_t index, uint32_t actor)
{
    uint16_t keys[1] = { index };
    Span16 span = { keys, 1, 1 };
    uint32_t err = 0xFFFFFFFF;
    t_skipSafeSpot = g_clanSafeSpot ? actor : 0;
    t_skipDisbandAck = g_disbandAck != NULL;
    t_disbandAckSkipped = false;
    g_removeAccompany(c->clan, &err, &span, 0, 0, 0, 0);
    t_skipSafeSpot = 0;
    t_skipDisbandAck = false;
    Log("  off the list -> %08X (%s)%s", err, ErrorText(err), t_disbandAckSkipped ? ", no disband notice to the client" : "");
    ReadClan(c);

    if (EntryOf(index))
        return false;

    if (t_disbandAckSkipped)
    {
        bool valid = true;

        for (uint32_t i = 0; i < g_listCount; ++i)
            valid &= g_list[i].index != 0xFFFF && g_list[i].actor != 0;

        SendListFn send = (SendListFn)R64(R64(c->clan) + 0x158);

        if (valid && (uintptr_t)send == g_base + SEND_LIST)
        {
            ListRef list = { (void*)R64(c->clan + 0xB8), R32(c->clan + 0xC0) };
            send(c->clan, &list);
            Log("  the new list (%u) sent to the client", list.count);
        }
        else
        {
            // The client would drop the list: the game's own notice after all.
            Span32 charKeys = { NULL, 0, 0 };
            uint32_t key = CharKeyOfIndex(&index);
            charKeys.data = &key;
            charKeys.count = 1;
            charKeys.cap = 1;
            g_disbandAck(c->clan, 0, 0, &charKeys);
            Log("  the list could not be sent (%s) - the game's notice sent", valid ? "unexpected sender" : "an odd entry");
        }
    }

    return true;
}

// Dismiss: off the list, still hired. The camp's own actor is removed softly
// (as the clan's vt+0x1A0 does, 0x2BAEB00: no disbanded flag, no unspawn, no
// safe-spot move), so she walks back to what she was doing; her leash stays
// until she is out of your sight. A summoned copy stays behind in Standby and
// gets the game's own unsummon (DisbandByKeys) when you are out of sight
// (StayBehind), or at once.
static void DismissHeld(Context* c, const Choice& ch, ULONGLONG now)
{
    uint16_t index = ch.index;
    uint32_t key = CharKeyOfIndex(&index);
    const char* name = NameOf(key);

    if (!name)
    {
        Log("dismiss: character %06X is not one of the Greymanes", key);
        return;
    }

    ReadClan(c);

    if (!EntryOf(index))
    {
        Log("dismiss: %s is not following you", name);
        return;
    }


    // Not while she is down or dead: the game's removal then handles her as a
    // fallen comrade (0x2152070 -> clan vt+0x1C8).
    int spawn = SpawnType(c, ch.target);
    Health health;
    ReadHealth(c, ch.target, HpKey(), &health);

    if (health.state == HEALTH_DOWN || health.state == HEALTH_DEAD)
    {
        Log("dismiss: %s is %s - dismiss her when she is up", name, health.state == HEALTH_DOWN ? "down" : "dead");
        return;
    }

    Log("dismiss: %s (%06X), actor %08X, spawn type %d", name, key, ch.target, spawn);

    if (spawn == SPAWN_SUMMONED && !g_stayBehind)
        Disband(c, key);
    else
        RemoveQuietly(c, index, ch.target);  // no safe-spot move (~200 m away), no client-side disband

    if (EntryOf(index))
    {
        Log("dismiss: %s is still on the list", name);
        return;
    }

    Comrade* m = Find(key);
    bool stays = spawn == SPAWN_SUMMONED && g_stayBehind;

    if (stays)
        Log("dismiss: %s stays where he is; he leaves when you are %.0f m away or travel", name, DEPART_OUT_OF_SIGHT_M);

    if (m)
        Release(c, m, stays ? "dismissed - stays behind" : "dismissed", now, true, stays);
    else if (stays)
        Depart(c, key, ch.target, 0, now, index, true, NULL);
    else
        Log("dismiss: %s dismissed", name);

    // Her AI starts again where she stands (her routine visible again), and
    // asks for her routine a second later: she walks back to it.
    if (spawn != SPAWN_SUMMONED)
    {
        Resync(c, ch.target, "dismissed");
        RaiseLater(ch.target, EVENT_RESET_SCHEDULE, "Reset schedule (her routine again)", now + 1000);
    }
}

static void ChoicesHeld(Context* c, ULONGLONG now)
{
    Choice taken[MAX_CHOICES];
    int n = TakeChoices(taken);

    for (int i = 0; i < n; ++i)
    {
        const Choice& ch = taken[i];

        if (now - ch.at > CHOICE_MS)
            Log("%s on actor %08X dropped: %llu ms old", OptionName(ch.option), ch.target, now - ch.at);
        else if (ch.option == RECRUIT_OPTION)
            RecruitHeld(c, ch, now);
        else
            DismissHeld(c, ch, now);
    }
}

// The player chose an interaction on a target (server). Only notes Recruit and
// Dismiss; the game's own interaction-by-index lookup is the one call made
// (TryPreempt itself makes it).
static uint64_t __fastcall HookTryPreempt(uint64_t self, uint64_t result, uint64_t target, uint64_t iidx, uint64_t a5,
                                          uint64_t a6, uint64_t a7, uint64_t a8)
{
    uint64_t r = g_tryPreempt(self, result, target, iidx, a5, a6, a7, a8);

    __try
    {
        if (result && R32(result) == 0 && target)
        {
            uint16_t i = (uint16_t)iidx;
            uintptr_t info = g_interactionByIndex(&i);
            uint32_t option = info ? R32(info) : 0;

            if (option == RECRUIT_OPTION || option == DISMISS_OPTION)
            {
                Choice ch = { option, R32(R64(self + 8) + 0x60), (uint32_t)R32(target + 0x60),
                              R16(R64(R64(target + 0x68) + 0x20) + 0x30), GetTickCount64() };
                PushChoice(ch);
                Log("choice: %s on actor %08X (character index %u)", OptionName(option), ch.target, ch.index);
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return r;
}

// ---------------------------------------------------------------------------
// The keys
// ---------------------------------------------------------------------------

// Bring back (F10): the comrades the plugin looks after come to you.
static void RescueHeld(Context* c, ULONGLONG now)
{
    if (now - g_lastRescue < RESCUE_COOLDOWN_MS)
    {
        Log("rescue: wait %llu s", (RESCUE_COOLDOWN_MS - (now - g_lastRescue)) / 1000 + 1);
        return;
    }

    g_lastRescue = now;
    bool any = false;

    for (Comrade& m : g_comrades)
    {
        if (m.phase == PHASE_FREE)
            continue;

        any = true;
        Log("rescue: %s [%s], actor %08X, %.1f m from you", Name(&m), PHASES[m.phase], m.actor, DistanceTo(c, m.actor));

        if (m.phase == PHASE_STANDBY)
        {
            Log("  rescue: she is in Standby - left where she is");
            continue;
        }

        if (!c->havePos || !ActorAlive(c, m.actor))
        {
            Log("  rescue: she is not there - the plugin registers her again by itself");
            continue;
        }

        BringBack(c, &m, now);
    }

    if (!any)
        Log("rescue: no comrade follows you - recruit one first");
}

static void Run(Request r, ULONGLONG now, bool pump, bool choices)
{
    Context c;

    __try
    {
        if (!Open(&c, r == REQUEST_NONE && !choices))
        {
            // Not now (a loading screen): a choice is not kept for later.
            if (choices)
            {
                Choice taken[MAX_CHOICES];
                int n = TakeChoices(taken);
                Log("%d talk option choice(s) dropped: the player is not there", n);
            }

            return;
        }

        __try
        {
            if (choices)
                ChoicesHeld(&c, now);

            if (r == REQUEST_RESCUE)
                RescueHeld(&c, now);

            if (pump)
                PumpHeld(&c, now);
        }
        __finally
        {
            Close(&c);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("request %d: an exception %08X - stopped", (int)r, GetExceptionCode());
    }
}

static bool OnServerThread()
{
    __try
    {
        uintptr_t tls = *(uintptr_t*)__readgsqword(0x58);
        return tls && R8(tls + 0x1EC) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// A key press waits for the server's next message, but not longer than this
// (not into the next loading screen).
static const ULONGLONG REQUEST_MS = 5000;

static void Serve(bool after)
{
    if (!OnServerThread() || InterlockedCompareExchange(&g_busy, 1, 0) != 0)
        return;

    ULONGLONG now = GetTickCount64();
    Request r = REQUEST_NONE;

    if (!after && g_request != REQUEST_NONE)
    {
        r = (Request)InterlockedExchange(&g_request, REQUEST_NONE);
        ULONGLONG age = now - g_requestAt;

        if (r != REQUEST_NONE && age > REQUEST_MS)
        {
            Log("request %d dropped: %llu ms old", (int)r, age);
            r = REQUEST_NONE;
        }
        else if (r != REQUEST_NONE)
            Log("request %d on the server thread (after %llu ms)", (int)r, age);
    }

    bool choices = g_choiceCount != 0;
    bool pump = !after && now - g_lastPump >= PUMP_MS;

    if (pump)
        g_lastPump = now;

    if (r != REQUEST_NONE || pump || choices)
        Run(r, now, pump, choices);

    InterlockedExchange(&g_busy, 0);
}

static uint64_t __fastcall HookDispatch(void* a, void* b, void* c, void* d, uint64_t e, uint64_t f)
{
    InterlockedIncrement(&g_calls);
    Serve(false);
    uint64_t r = g_dispatch(a, b, c, d, e, f);

    // A Recruit or Dismiss chosen in this message is acted on at once, before
    // the next message (a client's own answer to it) is dispatched.
    if (g_choiceCount)
        Serve(true);

    return r;
}

void GameRequest(Request r)
{
    g_requestAt = GetTickCount64();
    InterlockedExchange(&g_request, r);
}

// ---------------------------------------------------------------------------
// Probes (log only; they call no game function)
// ---------------------------------------------------------------------------

// The comrade's slot among the tracked keys, or -1.
static int TrackedSlot(uint32_t actorKey)
{
    if (!actorKey)
        return -1;

    for (int i = 0; i < MAX_COMRADES * 2; ++i)
        if (g_tracked[i] == actorKey)
            return i;

    return -1;
}


// The client AI's teleport request (packet 0x82B): its own result is 0 on
// almost every path, so the check's result is shown (0x2AC5BF0, below).
static uint64_t __fastcall ProbeRequest(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)
{
    uint32_t key = 0;

    __try
    {
        key = R32(R64(c + 0x18) + 5);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    t_checked = 0;
    t_checkErr = 0;
    uint64_t r = g_probeRequest(a, b, c, d, e, f);

    if (TrackedSlot(key) >= 0)
    {
        uint32_t own = R32(b);

        if (!g_checkProbe)
            Log("probe: teleport request for actor %08X (the check probe is off)%s%s", key, own ? ", handler error " : "",
                own ? ErrorText(own) : "");
        else if (t_checked != key)
            Log("probe: teleport request for actor %08X DROPPED before the check (actor, owner or AI)%s%s", key,
                own ? ", handler error " : "", own ? ErrorText(own) : "");
        else
            Log("probe: teleport request for actor %08X -> %08X (%s)", key, t_checkErr, ErrorText(t_checkErr));
    }

    return r;
}

static uint64_t __fastcall ProbeCheck(uint64_t ai, uint64_t err, uint64_t desc, uint64_t seq)
{
    uint32_t key = 0, controller = 0;
    uint64_t want = 0;

    __try
    {
        uintptr_t actor = R64(ai + 8);
        key = R32(actor + 0x60);
        controller = R32(actor + 0x90);
        want = R64(ai + 0x18);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    uint64_t r = g_probeCheck(ai, err, desc, seq);
    t_checked = key;
    t_checkErr = R32(err);

    if (TrackedSlot(key) >= 0)
    {
        float* p = (float*)desc;
        Log("probe: teleport check for actor %08X to %.1f %.1f %.1f, sequence %llX (wants %llX), controller %08X -> %08X (%s)",
            key, p[0], p[1], p[2], (unsigned long long)R64(seq), (unsigned long long)want, controller, t_checkErr,
            ErrorText(t_checkErr));
    }

    return r;
}

// The server-run AI's catch-up teleport (ServerAi vt+0x208); the plugin's own
// F10 fallback goes through it too.
static uint64_t __fastcall ProbeTeleport(uint64_t ai, uint64_t err, uint64_t token, uint64_t t)
{
    uint32_t key = 0;

    __try
    {
        key = R32(R64(ai + 8) + 0x60);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    uint64_t r = g_probeTeleport(ai, err, token, t);

    if (TrackedSlot(key) >= 0)
    {
        float* p = (float*)t;
        Log("probe: %s teleport of actor %08X to %.1f %.1f %.1f -> %08X (%s)", t_pluginTeleport ? "the plugin's" : "her AI's", key,
            p[0], p[1], p[2], R32(err), ErrorText(R32(err)));
    }

    return r;
}

// The clean-up's distance test. A 1 is not yet a logout: from the first test
// (0x2C37D5B) the clean-up may test again from the field owner (0x2C38EA7),
// and the protected 0x28F30C0 decides last. "She was logged out" comes from
// the status line, not from here.
static uint64_t __fastcall ProbeDistance(uint64_t actor, uint64_t ref, uint64_t c, uint64_t d)
{
    uint64_t r = g_probeDistance(actor, ref, c, d);
    uint32_t key = 0;

    __try
    {
        key = R32(actor + 0x60);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    int slot = TrackedSlot(key);

    if (slot < 0)
        return r;

    uintptr_t site = (uintptr_t)_ReturnAddress() - g_base;
    bool second = site == 0x2C38EA7;

    if (!(uint8_t)r && !second)
        return r;

    static volatile ULONGLONG last[MAX_COMRADES * 2][2];
    ULONGLONG now = GetTickCount64();

    if (now - last[slot][second] < 2000)
        return r;

    last[slot][second] = now;
    float leash = 0;
    float* p = (float*)ref;

    __try
    {
        leash = RF(R64(R64(actor + 0x68) + 0x20) + 0x468);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    Log("probe: the clean-up's %s distance test for actor %08X (from %.1f %.1f %.1f, logout distance %.0f) = %u",
        second ? "second (field owner)" : site == 0x2C37D5B ? "first" : "other", key, p ? p[0] : 0, p ? p[1] : 0, p ? p[2] : 0, leash,
        (unsigned)(uint8_t)r);
    return r;
}

// The server clan's safe-spot move: skipped only for the plugin's own Dismiss
// (same thread: 0x2151570 calls 0x2152070 calls this synchronously).
static void __fastcall SafeSpotDetour(uintptr_t clan, uintptr_t actor)
{
    uint32_t skip = t_skipSafeSpot;

    if (skip && actor)
    {
        __try
        {
            if (R32(actor + 0x60) == skip)
                return;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    g_clanSafeSpot(clan, actor);
}

// The clan's removal notice to the client: skipped only for the plugin's own
// Dismiss (same thread: 0x2151570 calls it at 0x2151FCD).
static void __fastcall DisbandAckDetour(uintptr_t clan, uint8_t a6, uint8_t a7, void* charKeys)
{
    if (t_skipDisbandAck)
    {
        t_disbandAckSkipped = true;
        return;
    }

    g_disbandAck(clan, a6, a7, charKeys);
}

// Log only: the server takes an actor out of the client's view (0x9D4).
static uint64_t __fastcall ProbeRemoveActor(uint64_t a, uint64_t b, uint64_t c, uint64_t d)
{
    __try
    {
        uintptr_t pkt = c ? R64(c + 0x18) : 0;
        uint16_t size = pkt ? R16(pkt + 3) : 0;

        for (uint32_t off = 5 + 6; pkt && off + 4 <= size && off < 5 + 6 + 64 * 4; off += 4)
        {
            uint32_t key = R32(pkt + off);

            if (TrackedSlot(key) >= 0)
                Log("probe: the server removed actor %08X from your view (reason %u)", key, R8(pkt + 10));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return g_probeRemoveActor(a, b, c, d);
}

// Research: why a switch to Kliff is refused - the game's own reason
// (UI_MercenaryQuickSlot_NonSwitchable_*, LocalStringInfoManager +0xCC..+0xEE).
static const char* SWITCH_REASONS[] = { "Wanted", "Crime", "ActiveSpecialMode", "ImportantQuest", "Target_ImportantQuest",
                                        "Interacting", "Action", "InvalidPosition", "Groggy (down, or the 10-minute call cooldown)",
                                        "Dead", "RidingVehicle", "Stealth", "Far", "NotAccompanying", "Operation", "Battle",
                                        "AlreadyCall", "NonCallable_AccompanyNotAllowed" };

static bool __fastcall SwitchDetour(uintptr_t focusActor, uint32_t targetKey, uint16_t* reason)
{
    bool ok = g_canSwitch(focusActor, targetKey, reason);

    if (ok || targetKey != 1)
        return ok;

    __try
    {
        static uint16_t lastReason = 0;
        static ULONGLONG lastAt = 0;
        uint16_t r = reason ? *reason : 0xFFFF;
        ULONGLONG now = GetTickCount64();

        if (r == lastReason && now - lastAt < 10000)
            return ok;

        lastReason = r;
        lastAt = now;
        bool server = false;
        uintptr_t tls = *(uintptr_t*)__readgsqword(0x58);
        server = tls && R8(tls + 0x1EC) != 0;
        uintptr_t m = R64(g_base + (server ? LOCAL_STRINGS_SERVER : LOCAL_STRINGS_CLIENT));
        m = m ? R64(m) : 0;
        m = m ? R64(m + 8) : 0;
        m = m ? R64(m + 0x1C0) : 0;
        const char* name = r == 0xFFFF ? "(no reason given)" : "?";

        for (int i = 0; m && i < (int)_countof(SWITCH_REASONS) && r != 0xFFFF; ++i)
            if (R16(m + 0xCC + i * 2) == r)
                name = SWITCH_REASONS[i];

        Log("switch to Kliff refused (%s side): %s", server ? "server" : "client", name);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return ok;
}

// ---------------------------------------------------------------------------
// HideRoutine: a recruited camp comrade's AI chart sees no daily routine
// (research/routine_dismiss_map_2026-10-03.txt). Her schedule task can only be
// left when its stay is over (CheckToDepartureNextNPCSchedule), and the
// chart's re-decisions prefer the schedule to following; so for her these AI
// conditions answer as with no schedule, and departure answers "leave now".
// Their vtable slots are swapped (the conditions' code itself is protected).
// Answers: 0 true, 1 false.
// ---------------------------------------------------------------------------

typedef uint8_t (__fastcall* CondEvalFn)(uintptr_t self, uintptr_t runner, uintptr_t ctx);

struct CondSlot { uintptr_t slot, original, typeFn; uint16_t typeId; uint8_t forced; bool optional; const char* name; };
static const CondSlot COND_SLOTS[] = {
    { 0x58C01A8, 0x22E38B0, 0x22E38A0, 0x217, 0, false, "departure" },        // CheckToDepartureNextNPCSchedule
    { 0x58C2208, 0x22E3200, 0x22E31F0, 0x20B, 1, false, "exist" },            // CheckExistNPCScheduleInfo
    { 0x58C0170, 0x22E39B0, 0x22E39A0, 0x21A, 1, false, "location" },         // CheckCurrentNPCScheduleLocationType
    { 0x58C0288, 0x22E3A50, 0x22E3A40, 0x21B, 1, false, "main" },             // CheckMainScheduleType
    { 0x58C0058, 0x22E3F10, 0x22E3F00, 0x225, 1, false, "next location" },    // CheckNextNPCScheduleLocationType
    { 0x58BFED0, 0x22E4710, 0x22E4700, 0x22E, 1, false, "spawn action" },     // CheckNPCScheduleSpawnAction
    { 0x58BFCA0, 0x22E49B0, 0x22E49A0, 0x230, 1, true, "initial control" },   // CheckNPCScheduleInitialAIControlType
};
static const int COND_COUNT = (int)(sizeof(COND_SLOTS) / sizeof(COND_SLOTS[0]));
static const uintptr_t COND_EVALUATE_CALL = 0x22D5FE1;   // the generic evaluator's vt+8 call (rcx cond, rdx runner, r8 ctx)
static const uintptr_t VT_CLIENT_AI = 0x55B8448;         // .?AVClientAiActorComponent@pa@@

static CondEvalFn g_condOrig[COND_COUNT];
static volatile LONG g_condForced[COND_COUNT];
static LONG g_condForcedSeen[COND_COUNT];

// One record per condition instance that was overridden (logged by the pump).
struct CondSeen { volatile LONG64 self; uint32_t key, aiVt; uint8_t idx, param, orig, scheduleState; volatile LONG ready; };
static CondSeen g_condSeen[32];
static bool g_condLogged[32];

// The runner's AI component ([[runner+0x68]], as CheckAccompany 0x22EA744)
// and its actor's key; 0 when anything is off.
static __declspec(noinline) uint32_t RunnerKey(uintptr_t runner, uintptr_t* aiOut)
{
    __try
    {
        uintptr_t h = R64(runner + 0x68);
        uintptr_t ai = h ? R64(h) : 0;
        uintptr_t actor = ai ? R64(ai + 8) : 0;
        *aiOut = ai;
        return actor ? R32(actor + 0x60) : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

static __declspec(noinline) void NoteCond(int idx, uintptr_t self, uintptr_t ai, uint32_t key, uint8_t orig)
{
    for (CondSeen& e : g_condSeen)
        if ((uintptr_t)e.self == self)
            return;

    for (CondSeen& e : g_condSeen)
        if (InterlockedCompareExchange64(&e.self, (LONG64)self, 0) == 0)
        {
            e.idx = (uint8_t)idx;
            e.key = key;
            e.orig = orig;

            __try
            {
                e.param = idx == 2 ? R8(self + 8) : 0;
                e.aiVt = (uint32_t)(R64(ai) - g_base);
                uintptr_t sched = R64(ai + 0x90);
                e.scheduleState = sched ? R8(sched + 0x66) : 0xFF;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }

            InterlockedExchange(&e.ready, 1);
            return;
        }
}

template <int I>
static uint8_t __fastcall CondDetour(uintptr_t self, uintptr_t runner, uintptr_t ctx)
{
    uint8_t r = g_condOrig[I](self, runner, ctx);

    if (!g_hiddenCount)
        return r;

    uintptr_t ai = 0;
    uint32_t key = RunnerKey(runner, &ai);

    if (!key)
        return r;

    bool hidden = false;

    for (int i = 0; i < MAX_COMRADES; ++i)
        hidden |= (uint32_t)g_hidden[i] == key;

    if (!hidden)
        return r;

    uint8_t f = COND_SLOTS[I].forced;

    if (r != f)
    {
        InterlockedIncrement(&g_condForced[I]);
        NoteCond(I, self, ai, key, r);
    }

    return f;
}

static void* const COND_DETOURS[] = { (void*)&CondDetour<0>, (void*)&CondDetour<1>, (void*)&CondDetour<2>, (void*)&CondDetour<3>,
                                      (void*)&CondDetour<4>, (void*)&CondDetour<5>, (void*)&CondDetour<6> };
static_assert(sizeof(COND_DETOURS) / sizeof(COND_DETOURS[0]) == sizeof(COND_SLOTS) / sizeof(COND_SLOTS[0]), "one detour per slot");

// A vtable entry swapped for a detour (compare-and-swap; the tables are data).
static bool SwapSlot(uintptr_t slotRva, uintptr_t expectRva, void* detour, void** original, const char* name)
{
    void** slot = (void**)(g_base + slotRva);
    DWORD old;

    if (*slot != (void*)(g_base + expectRva) || !VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old))
    {
        Log("%s: the table entry is not as expected - off", name);
        return false;
    }

    void* seen;

    do
    {
        seen = *slot;
        *original = seen;
    } while (InterlockedCompareExchangePointer(slot, detour, seen) != seen);

    VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

static bool CondSlotOk(const CondSlot& k)
{
    const uint8_t want[6] = { 0xB8, (uint8_t)(k.typeId & 0xFF), (uint8_t)(k.typeId >> 8), 0, 0, 0xC3 };
    return memcmp((const void*)(g_base + k.typeFn), want, 6) == 0 && R64(g_base + k.slot) == g_base + k.original;
}

static void InstallRoutineHide()
{
    if (!Matches(COND_EVALUATE_CALL, "4D8BC6 498BD5 498BCF FF5008"))
    {
        Log("routine hide: the condition evaluator does not look as expected - off");
        return;
    }

    for (const CondSlot& k : COND_SLOTS)
        if (!k.optional && !CondSlotOk(k))
        {
            Log("routine hide: the %s condition is not as expected - off", k.name);
            return;
        }

    int swapped = 0;

    for (int i = 0; i < COND_COUNT; ++i)
    {
        if (COND_SLOTS[i].optional && !CondSlotOk(COND_SLOTS[i]))
        {
            Log("routine hide: the %s condition is not as expected - left out", COND_SLOTS[i].name);
            continue;
        }

        swapped += SwapSlot(COND_SLOTS[i].slot, COND_SLOTS[i].original, COND_DETOURS[i], (void**)&g_condOrig[i], COND_SLOTS[i].name);
    }

    g_condInstalled = swapped >= COND_COUNT - 1;
    Log("routine hide: %d of %d conditions swapped", swapped, COND_COUNT);
}

// ---------------------------------------------------------------------------
// MapIcon: recruited Greymanes on the map and minimap. The client's icon
// choice for an actor (0x9E8D10) gives a camp comrade none (_playerIndex 0,
// the mercenary branch 0x9E99B1); for the plugin's comrades the party icon
// is filled in where the game chose nothing. Writes only the caller's local.
// ---------------------------------------------------------------------------

static const uintptr_t MAP_ICON_SELECT = 0x9E8D10, MAP_ICON_CALL = 0x9E8A6B;
static const uintptr_t VT_MAP_TEXTURES = 0x59881A0;      // .?AVUIMapTextureInfoManager@pa@@ (-> -> +8 -> +0x270)
static const uintptr_t MAP_TEX_CHARACTER = 0x100, MAP_TEX_PARTY = 0x104;   // Actor_Character, Actor_Party_Default
static const uintptr_t MINIMAP_UPDATE = 0x9E7C60, SLOT_MINIMAP_UPDATE = 0x55BEC90 + 0xF8;  // ClientMinimapActorComponent

typedef uint64_t (__fastcall* MapIconFn)(uintptr_t comp, uintptr_t focus, uintptr_t owner, uint16_t* tex, uint8_t* noIcon,
                                         uint8_t* groggy, uint8_t* removed, uint8_t* relation);
typedef void (__fastcall* MinimapUpdateFn)(uintptr_t comp);
static MapIconFn g_mapIconSelect;
static MinimapUpdateFn g_minimapUpdate;

// The comrade slot (0..MAX_COMRADES-1) of a client actor, or -1.
static __declspec(noinline) int MapSlot(uintptr_t comp, uintptr_t* actorOut)
{
    __try
    {
        uintptr_t actor = R64(comp + 8);
        uint32_t key = actor ? R32(actor + 0x60) : 0;
        *actorOut = actor;

        if (!key)
            return -1;

        for (int i = 0; i < MAX_COMRADES; ++i)
            if (g_tracked[i] == key)
                return i;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return -1;
}

static uint64_t __fastcall MapIconDetour(uintptr_t comp, uintptr_t focus, uintptr_t owner, uint16_t* tex, uint8_t* noIcon,
                                         uint8_t* groggy, uint8_t* removed, uint8_t* relation)
{
    uint64_t r = g_mapIconSelect(comp, focus, owner, tex, noIcon, groggy, removed, relation);
    uintptr_t actor = 0;
    int slot = MapSlot(comp, &actor);

    if (slot < 0)
        return r;

    InterlockedIncrement(&g_mapCalls[slot]);

    __try
    {
        uint16_t orig = tex ? *tex : 0xFFFF;
        uint16_t t = 0xFFFF;

        // The game chose an icon, hid it or removed it: left as it is. Dead:
        // no party icon on a body.
        if (tex && orig == 0xFFFF && !(noIcon && *noIcon) && !(removed && *removed) && !R8(R64(R64(actor + 0x68) + 0x20) + 0x273))
        {
            uintptr_t tls = *(uintptr_t*)__readgsqword(0x58);
            uintptr_t m = R64(g_base + (tls && R8(tls + 0x1EC) ? LOCAL_STRINGS_SERVER : LOCAL_STRINGS_CLIENT));
            m = m ? R64(m) : 0;
            m = m ? R64(m + 8) : 0;
            m = m ? R64(m + 0x270) : 0;

            if (m && R64(m) == g_base + VT_MAP_TEXTURES)
            {
                t = R16(m + g_mapIconOffset);

                if (t != 0xFFFF)
                    *tex = t;   // never *noIcon: 1 there means "no icon" (0x9E90AA, 0x9E9FDB)
            }
        }

        MapDiag& g = g_mapDiag[slot];
        uintptr_t* ret = (uintptr_t*)_AddressOfReturnAddress();
        g.caller = *ret == g_base + 0x9E8A70 ? (uint32_t)(*(uintptr_t*)((char*)ret + 0xD0) - g_base) : 0;
        g.shown = R16(comp + 0x18);
        g.shown2 = R16(comp + 0x1A);
        g.bits34 = R8(comp + 0x34);
        g.bits35 = R8(comp + 0x35);
        g.cache = R16(comp + 0x38);
        g.cache2 = R16(comp + 0x3A);
        g.groggy = R8(comp + 0x3C);
        g.hide = R8(comp + 0x3D);
        g.pending = R8(comp + 0x3E);
        g.age = (int32_t)(R32(g_base + 0x6C8CCC0) - R32(comp + 0x40));
        g.relation = relation ? *relation : 0xFF;
        g.mode = R8(g_base + 0x6CC1A98);
        InterlockedIncrement(&g.seq);

        MapSeen& e = g_mapSeen[slot];

        if ((uint32_t)e.key != R32(actor + 0x60))
        {
            e.origTex = orig;
            e.tex = t;
            e.noIcon = noIcon ? *noIcon : 0xFF;
            e.removed = removed ? *removed : 0xFF;
            e.kind = R8(R64(actor + 0x88) + 1);
            e.bits34 = R8(comp + 0x34);
            InterlockedExchange(&e.key, (LONG)R32(actor + 0x60));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return r;
}

// MapIconRefresh (untested): asks the minimap to choose a comrade's icon again.
static void MapRefreshBits(uintptr_t comp)
{
    uintptr_t actor = 0;

    if (MapSlot(comp, &actor) >= 0)
    {
        __try
        {
            if (!(R8(comp + 0x34) & 6))
                *(volatile uint8_t*)(comp + 0x34) |= 2;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }
}

static void __fastcall MinimapUpdateDetour(uintptr_t comp)
{
    MapRefreshBits(comp);
    g_minimapUpdate(comp);
}

static void InstallMapIcon()
{
    if (!Matches(MAP_ICON_SELECT, "48895C2420 4C89442418 4889542410 48894C2408 55 56 57 4154 4155 4156 4157 488DAC2460F8FFFF 4881ECA0080000") ||
        CallTarget(MAP_ICON_CALL) != g_base + MAP_ICON_SELECT)
    {
        Log("map icon: the map's icon choice does not look as expected - off");
        return;
    }

    void* target = (void*)(g_base + MAP_ICON_SELECT);
    MH_STATUS st;

    if ((st = MH_CreateHook(target, (void*)&MapIconDetour, (void**)&g_mapIconSelect)) != MH_OK || (st = MH_EnableHook(target)) != MH_OK)
    {
        Log("map icon: could not hook (%d)", (int)st);
        return;
    }

    if (g_mapIconRefresh && !g_horses)   // with Horses, the horse tick does it
        SwapSlot(SLOT_MINIMAP_UPDATE, MINIMAP_UPDATE, (void*)&MinimapUpdateDetour, (void**)&g_minimapUpdate, "map icon refresh");

    Log("map icon: hooked (%s icon%s)", g_mapIconOffset == MAP_TEX_PARTY ? "party" : "character", g_mapIconRefresh ? ", refresh on" : "");
}

// What the detours saw, written to the log on the server thread.
static const char* NameOfActor(uint32_t actorKey)
{
    for (const Comrade& m : g_comrades)
        if (m.phase != PHASE_FREE && m.actor == actorKey)
            return Name(&m);

    return "?";
}

static void LogSeen(ULONGLONG now)
{
    (void)now;

    for (int i = 0; i < (int)_countof(g_condSeen); ++i)
    {
        CondSeen& e = g_condSeen[i];

        if (!e.ready || g_condLogged[i])
            continue;

        g_condLogged[i] = true;
        Log("routine: %s's %s condition (%p, type %u) answered %u, now %u; her AI %s (vt %X), schedule state %u", NameOfActor(e.key),
            COND_SLOTS[e.idx].name, (void*)(uintptr_t)e.self, e.param, e.orig, COND_SLOTS[e.idx].forced,
            e.aiVt == VT_CLIENT_AI ? "client" : e.aiVt == VT_SERVER_AI ? "server" : "other", e.aiVt, e.scheduleState);
    }

    for (int i = 0; i < MAX_COMRADES; ++i)
    {
        MapSeen& e = g_mapSeen[i];
        LONG key = e.key;

        if (!key || key == g_mapLogged[i] || (uint32_t)key != g_tracked[i])
            continue;

        g_mapLogged[i] = key;
        Log("map: %s - the game chose icon %04X (no icon %u, removed %u), now %04X; kind %u, minimap bits %02X", NameOfActor((uint32_t)key),
            e.origTex, e.noIcon, e.removed, e.tex, e.kind, e.bits34);
    }
}

static void LogSeenStatus()
{
    if (g_hiddenCount)
    {
        LONG d[COND_COUNT];

        for (int i = 0; i < COND_COUNT; ++i)
        {
            LONG v = g_condForced[i];
            d[i] = v - g_condForcedSeen[i];
            g_condForcedSeen[i] = v;
        }

        Log("  routine hidden for %ld: departure %ld, exist %ld, location %ld, main %ld, next location %ld, spawn action %ld,"
            " initial control %ld answers changed", (long)g_hiddenCount, d[0], d[1], d[2], d[3], d[4], d[5], d[6]);
    }

    if (g_mapIconSelect)
    {
        LONG d[MAX_COMRADES];

        for (int i = 0; i < MAX_COMRADES; ++i)
        {
            LONG v = g_mapCalls[i];
            d[i] = v - g_mapCallsSeen[i];
            g_mapCallsSeen[i] = v;
        }

        char calls[96] = "";

        for (int i = 0; i < MAX_COMRADES; ++i)
            sprintf_s(calls + strlen(calls), sizeof(calls) - strlen(calls), i ? "/%ld" : "%ld", d[i]);

        Log("  map: icon choices for the comrades %s", calls);

        for (int i = 0; i < MAX_COMRADES; ++i)
        {
            const MapDiag& g = g_mapDiag[i];

            if (!g_tracked[i] || g.seq == g_mapDiagSeen[i])
                continue;

            g_mapDiagSeen[i] = g.seq;
            Log("  map: %s caller %X shown %04X/%04X bits34 %02X bits35 %02X cache %04X/%04X g%u h%u pending %u age %d relation %u"
                " mode %u", NameOfActor(g_tracked[i]), g.caller, g.shown, g.shown2, g.bits34, g.bits35, g.cache, g.cache2, g.groggy,
                g.hide, g.pending, g.age, g.relation, g.mode);
        }
    }
}

// ---------------------------------------------------------------------------
// Horses (experiment): while you ride, each following comrade rides too. A
// free horse near her is borrowed, else one is spawned for her (a plain
// actor, no mercenary record: nothing for the save); she is seated with the
// game's own sequencer ride-on (client 0x5EE330 -> packet 0xA1D to the server)
// and taken off again (vehicle vt+0x158, packet 0xB7A) when you get off; a
// spawned horse is then logged out. Research: research/horses_2026-10-03.txt.
// ---------------------------------------------------------------------------

static const uintptr_t ACTOR_SNAPSHOT = 0x2A83CB0;    // (server actor manager, VecRef* out): held refs of every character actor
static const uintptr_t RIDE_ON_CLIENT = 0x5EE330;     // (0, ClientActor* sender, ClientActor* rider, ClientActor* vehicle)
static const uintptr_t CLIENT_WORLD = 0x6D694D8;      // -> +0x10 the client actor manager
static const uintptr_t VT_CLIENT_ACTORS = 0x55B62C8, CLIENT_FIND_ACTOR = 0x8AE1A0;   // vt+0x28 (mgr, Scope*, key)
static const uintptr_t DESC_INIT = 0x278EA70, DESC_POSITION = 0x2797080, DESC_FREE = 0x278F260, SPAWN_ROTATION = 0x6A65908;
static const uintptr_t LOGOUT = 0x2068340, ERR_LOGOUT_DISMOUNT = 0x6CF71D4;
static const uintptr_t CHARACTER_KEYS = 0x6D69A48;    // key -> index table
static const uint32_t HORSE_REFERENCE = 0xF46D5;      // Damiane's horse: its vehicle type is the horses'
static const uint32_t HORSE_TO_SPAWN = 0x7B0B;        // Animal_Davrella_Domestic_Saddle_31499
static const float HORSE_NEAR_M = 25.0f, RIDE_NEAR_M = 40.0f, HORSE_ARRIVED_M = 6.0f;
static const uintptr_t INTERACTION_PREEMPT = 0x2B36860;   // (interaction comp, u32* res, u32* target, u32* interaction, pivot, f5, extra*)
static const uintptr_t VT_SERVER_INTERACTION = 0x5B2AC00;  // [[actor+0x68]+0x98]
static const uintptr_t EVENT_TAG_NEW_SUMMONED = 0x17B5D90; // u32 (): the "NewSummoned" event tag
static const uint32_t INTERACTION_RIDE_MERCENARY = 0xF428A; // Horse_Ride_Mercenary (no ownership condition)
static const uint32_t EVENT_CALLING_VEHICLE = 0x1813514E;   // OnCallingVehicle: the horse comes to the caller
static const uint32_t EVENT_PREEMPTION_START = 0xD2A12B93;  // OnInteraction_Preemption_Start: her AI waits for the interaction
static const ULONGLONG PREEMPT_AFTER_START_MS = 400;
static const ULONGLONG HORSE_CALL_MS = 6000, MOUNT_WAIT_MS = 12000;

struct ActorRef { uintptr_t vt, actor; uint8_t valid; uint8_t pad[15]; };
struct ActorSnapshot { ActorRef* data; uint32_t count, cap; ActorRef inl[0x1000]; };
static ActorSnapshot g_snapshot;

// Ride requests for the client thread (written by the server pump).
struct RideReq { volatile LONG state; uint32_t rider, horse; int tries; uint32_t err; };   // 1 pending, 2 done, 3 failed
static RideReq g_rideReq[MAX_COMRADES];
static MinimapUpdateFn g_clientTickOrig;
static ULONGLONG g_lastHorseLog = 0;

static uint16_t IndexOfKey(uint32_t key)
{
    __try
    {
        uintptr_t t = R64(g_base + CHARACTER_KEYS);
        uint32_t n = t ? R32(t + 0x68) : 0;

        if (!t || !R32(t + 0x6C) || !n)
            return 0xFFFF;

        uintptr_t bucket = R64(t + 0x78) + (uintptr_t)(key % n) * 0x100;
        uint32_t count = R32(bucket);

        for (uint32_t i = 0; i < count && i < 31; ++i)
            if (R32(bucket + 8 + i * 8) == key)
            {
                uintptr_t e = R64(R64(t + 0x80) + (uintptr_t)R32(bucket + 0xC + i * 8) * 8);

                if (R32(e + 4) == key)
                    return R16(e + 8);
            }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    return 0xFFFF;
}

static uint16_t HorseVehicleType()
{
    static uint16_t type = 0;

    if (!type)
    {
        uint16_t index = IndexOfKey(HORSE_REFERENCE);
        uintptr_t ci = index != 0xFFFF ? g_charInfo(&index) : 0;
        type = ci ? R16(ci + 0x6E) : 0xFFFF;
        Log("horses: the horses' vehicle type is %04X", type);
    }

    return type;
}

static bool HorseTaken(uint32_t key)
{
    for (const Comrade& m : g_comrades)
        if (m.phase != PHASE_FREE && m.horse == key)
            return true;

    return false;
}

// A free horse within 25 m of her (riderless, not someone's mercenary), 0 if none.
static uint32_t FindFreeHorse(Context* c, const Comrade* m)
{
    uintptr_t mgr = R64(R64(g_base + SERVER_ACTORS));
    uint16_t type = HorseVehicleType();

    if (!mgr || type == 0xFFFF || !m->havePos || R32(mgr + 0xC0) > _countof(g_snapshot.inl))
        return 0;

    g_snapshot.data = g_snapshot.inl;
    g_snapshot.count = 0;
    g_snapshot.cap = _countof(g_snapshot.inl);
    VecRef out = { &g_snapshot, NULL };
    typedef void (__fastcall* SnapshotFn)(uintptr_t mgr, VecRef* out);
    ((SnapshotFn)(g_base + ACTOR_SNAPSHOT))(mgr, &out);
    uint32_t best = 0, seen = 0;
    float bestD = HORSE_NEAR_M;

    for (uint32_t i = 0; i < g_snapshot.count && g_snapshot.data == g_snapshot.inl; ++i)
    {
        const ActorRef& e = g_snapshot.data[i];

        __try
        {
            uintptr_t a = e.valid ? e.actor : 0;
            uintptr_t vc = a ? R64(R64(a + 0x68) + 0x48) : 0;

            if (!vc || !R8(vc + 0xEB) || R32(vc + 0x48) || R8(vc + 0xC8) || R32(vc + 0x88) || R8(vc + 0x21))
                continue;

            uint16_t index = R16(R64(R64(a + 0x68) + 0x20) + 0x30);
            uintptr_t ci = g_charInfo(&index);

            if (!ci || R16(ci + 0x6E) != type)
                continue;

            ++seen;
            uintptr_t merc = R64(R64(a + 0x68) + 0x118);
            uint32_t key = R32(a + 0x60);
            float x, y, z;

            if ((merc && R32(merc + 0x1C)) || HorseTaken(key) || !WorldPos(a, &x, &y, &z))
                continue;

            float d = Dist(x, y, z, m->x, m->y, m->z);

            if (d < bestD)
            {
                bestD = d;
                best = key;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    for (uint32_t i = g_snapshot.count; i-- > 0 && g_snapshot.data == g_snapshot.inl;)
    {
        ActorRef& e = g_snapshot.data[i];
        typedef void (__fastcall* RefReleaseFn)(ActorRef* ref, int flags);

        if (e.vt)
            ((RefReleaseFn)R64(e.vt))(&e, 0);
    }

    if (g_snapshot.data != g_snapshot.inl)
        Log("horses: the actor list grew past %u - not released", g_snapshot.cap);

    Log("horses: %u actors, %u free horses; %s", g_snapshot.count, seen, best ? "one near her" : "none within 25 m");
    return best;
}

// A horse spawned next to her (the dev spawn's steps, 0x2C3F0D6..0x2C3F33D;
// type Summon, she is its spawner). Its actor key, 0 on failure.
static uint32_t SpawnHorse(Context* c, const Comrade* m)
{
    uint16_t index = IndexOfKey(HORSE_TO_SPAWN);
    uintptr_t ci = index != 0xFFFF ? g_charInfo(&index) : 0;

    if (!ci || !m->havePos)
    {
        Log("horses: no horse to spawn (index %04X)", index);
        return 0;
    }

    Scope s = {};
    g_findActor(c->mgr, &s, m->actor);
    uintptr_t rider = Held(&s);
    uint32_t key = 0;

    if (rider)
    {
        __declspec(align(16)) uint8_t fs[0x40] = {};
        g_fieldScope(rider, fs);

        __try
        {
            if (fs[0x10])
            {
                uintptr_t field = *(uintptr_t*)(fs + 8);
                __declspec(align(16)) static uint8_t desc[0x1000];
                memset(desc, 0, sizeof(desc));
                typedef void (__fastcall* DescInitFn)(uint8_t* desc, uint64_t zero, uint8_t reason);
                typedef void (__fastcall* DescPosFn)(uint8_t* desc, const float* pos, uintptr_t rotation);
                typedef void (__fastcall* DescFreeFn)(uint8_t* desc);
                ((DescInitFn)(g_base + DESC_INIT))(desc, 0, 0x0D);
                float off = g_horseMount ? 6.0f : 2.0f;   // HorseMount: she walks to it
                float pos[3] = { m->x + off, m->y, m->z + off };
                ((DescPosFn)(g_base + DESC_POSITION))(desc, pos, g_base + SPAWN_ROTATION);
                *(uint16_t*)(desc + 8) = index;
                *(uint32_t*)(desc + 0x3C0) = m->actor;

                if (R8(ci + 0x68) == 4 && c->owner)
                {
                    typedef uint64_t (__fastcall* GetFn)(uintptr_t actor);
                    *(uint64_t*)(desc + 0x40) = ((GetFn)R64(R64(c->owner) + 0x160))(c->owner);
                }

                uint32_t err = 0xFFFFFFFF;
                typedef void (__fastcall* CheckFn)(uint8_t* desc, uint32_t* err);
                ((CheckFn)R64(R64((uintptr_t)desc) + 0x20))(desc, &err);

                if (err == 0)
                {
                    uint8_t* one[1] = { desc };
                    struct { uint8_t** p; uint32_t n; } span = { one, 1 };
                    uint32_t err2 = 0xFFFFFFFF;
                    typedef uint32_t* (__fastcall* SpawnFn)(uintptr_t field, uint32_t* err, void* span, void* out);
                    ((SpawnFn)R64(R64(field) + 0x88))(field, &err2, &span, NULL);
                    uintptr_t horse = *(uintptr_t*)(desc + 0x18);
                    key = err2 == 0 && desc[0x20] && horse ? R32(horse + 0x60) : 0;
                    Log("horses: spawn -> %08X (%s), actor %08X (kind of character %u)", err2, ErrorText(err2), key, R8(ci + 0x68));

                    if (key)
                        g_setLogoutDistance(horse, LEASH_M);
                }
                else
                    Log("horses: the spawn was refused -> %08X (%s)", err, ErrorText(err));

                ((DescFreeFn)(g_base + DESC_FREE))(desc);
            }
            else
                Log("horses: her field was not found");
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            Log("horses: the spawn failed (exception)");
        }

        if (fs[0x10])
        {
            uintptr_t obj = *(uintptr_t*)(fs + 0x28);
            typedef void (__fastcall* DetachFn)(uintptr_t obj);
            ((DetachFn)R64(R64(obj) + 0x148))(obj);
        }

        g_scopeRelease(fs);
    }

    g_scopeRelease(&s);
    return key;
}

// The vehicle the comrade is on, per the game (0 on foot).
static uint32_t RidingOf(Context* c, uint32_t actorKey)
{
    uint32_t on = 0;
    Scope s = {};
    g_findActor(c->mgr, &s, actorKey);

    __try
    {
        uintptr_t a = Held(&s);
        uintptr_t rc = a ? R64(R64(a + 0x68) + 0x48) : 0;
        uintptr_t block = rc ? R64(rc + 0x18) : 0;
        on = block ? R32(block + 0x10) : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    g_scopeRelease(&s);
    return on;
}

// Off a horse: the vehicle's ride-off with the plain style (2: her AI gets
// OnRideOff_ByChangeFocusActor; 0 is a passenger's, 1/3 the dead fall -
// 0x2229871..0x22298DF), sent to the client (0xB7A).
static void RideOff(Context* c, Comrade* m, uint32_t horseKey, const char* why)
{
    Scope s = {};
    g_findActor(c->mgr, &s, horseKey);

    __try
    {
        uintptr_t horse = Held(&s);
        uintptr_t vc = horse ? R64(R64(horse + 0x68) + 0x48) : 0;

        if (vc)
        {
            uint32_t err = 0xFFFFFFFF, rider = m->actor;
            typedef void (__fastcall* RideOffFn)(uintptr_t vc, uint32_t* err, const uint32_t* rider, uint32_t p4, uint8_t p5, uint8_t p6);
            ((RideOffFn)R64(R64(vc) + 0x158))(vc, &err, &rider, 0, 0, 2);
            Log("horses: %s off horse %08X (%s) -> %08X (%s)", Name(m), horseKey, why, err, ErrorText(err));
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("horses: getting off failed (exception)");
    }

    g_scopeRelease(&s);
}

// Off the horse (server: the vehicle's ride-off, sent to the client), and a
// spawned horse logged out.
static void DropHorse(Context* c, Comrade* m, const char* why, bool remove)
{
    Scope s = {};
    g_findActor(c->mgr, &s, m->horse);

    __try
    {
        uintptr_t horse = Held(&s);
        uintptr_t vc = horse ? R64(R64(horse + 0x68) + 0x48) : 0;

        if (vc)
        {
            // Still on it: the vehicle's ride-off with the plain style (2: her AI
            // gets OnRideOff_ByChangeFocusActor; 0 is a passenger's, 1/3 the dead
            // fall - 0x2229871..0x22298DF).
            uint32_t on = 0;
            Scope rs = {};
            g_findActor(c->mgr, &rs, m->actor);
            uintptr_t a = Held(&rs);
            uintptr_t rc = a ? R64(R64(a + 0x68) + 0x48) : 0;
            uintptr_t block = rc ? R64(rc + 0x18) : 0;
            on = block ? R32(block + 0x10) : 0;
            g_scopeRelease(&rs);

            if (on == m->horse)
            {
                uint32_t err = 0xFFFFFFFF, rider = m->actor;
                typedef void (__fastcall* RideOffFn)(uintptr_t vc, uint32_t* err, const uint32_t* rider, uint32_t p4, uint8_t p5, uint8_t p6);
                ((RideOffFn)R64(R64(vc) + 0x158))(vc, &err, &rider, 0, 0, 2);
                Log("horses: %s off horse %08X (%s) -> %08X (%s)", Name(m), m->horse, why, err, ErrorText(err));
            }
            else
                Log("horses: %s is off horse %08X already (%s)", Name(m), m->horse, why);

            if (!remove && m->horseSpawned)
                g_setLogoutDistance(horse, LOGOUT_DEFAULT_M);   // left behind, it goes when you are far, as Damiane's

            if (remove && m->horseSpawned)
            {
                uint8_t out[0x20] = {};
                typedef void (__fastcall* LogoutFn)(uintptr_t actor, uint8_t* out, uint8_t type, uint32_t reason);
                ((LogoutFn)(g_base + LOGOUT))(horse, out, 0x2B, R32(g_base + ERR_LOGOUT_DISMOUNT));
                Log("horses: the spawned horse %08X logged out", m->horse);
            }
        }
        else
            Log("horses: %s's horse %08X is gone (%s)", Name(m), m->horse, why);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("horses: getting off failed (exception)");
    }

    g_scopeRelease(&s);

    for (RideReq& r : g_rideReq)
        if (r.rider == m->actor)
            InterlockedExchange(&r.state, 0);

    m->offSince = 0;
    m->reseats = 0;

    if (remove || !m->horseSpawned)
    {
        m->horse = 0;
        m->horseSpawned = false;
    }
}

// Her horse brought next to her (a far horse, as Damiane's comes when called).
static void HorseToHer(Context* c, Comrade* m)
{
    Scope s = {};
    g_findActor(c->mgr, &s, m->horse);
    uintptr_t h = Held(&s);
    uintptr_t pc = h ? R64(R64(h + 0x68) + 0x1A0) : 0;

    if (pc && m->havePos)
    {
        __declspec(align(16)) uint8_t t[0x200] = {};
        g_teleportInit(t);
        t[0xE8] = 0;
        *(uint64_t*)(t + 0xEC) = 0;
        t[0xF4] = 0;
        float* q = (float*)t;
        q[0] = m->x + 2.0f;
        q[1] = m->y;
        q[2] = m->z + 2.0f;
        uint32_t err[4] = {};
        g_teleport(pc, err, t);
        Log("horses: %s's horse %08X brought to her -> %08X (%s)", Name(m), m->horse, err[0], ErrorText(err[0]));
    }

    g_scopeRelease(&s);

    if (pc)
        Resync(c, m->horse, "her horse");
}

static void QueueSeat(Context* c, Comrade* m, int slot)
{
    m->kicked = false;
    m->dismountAt = 0;

    if (m->horseSpawned)
        LogoutDistance(c, m->horse, true, LEASH_M);

    RideReq& r = g_rideReq[slot];
    r.rider = m->actor;
    r.horse = m->horse;
    r.tries = 0;
    r.err = 0;
    InterlockedExchange(&r.state, 1);
}

static float HorseDistance(Context* c, const Comrade* m)
{
    float d = -1;
    Scope hs = {};
    g_findActor(c->mgr, &hs, m->horse);
    uintptr_t h = Held(&hs);
    float x, y, z;

    if (h && m->havePos && WorldPos(h, &x, &y, &z))
        d = Dist(x, y, z, m->x, m->y, m->z);

    g_scopeRelease(&hs);
    return d;
}

// An AI event on an actor with another actor as its target (request +0x18 /
// +0x1C), as the client raises OnInteraction_Preemption_Start (0x9B5EB0).
static bool RaiseEventOn(Context* c, uint32_t actorKey, uint32_t hash, uint32_t target, const char* name)
{
    Scope s = {};
    g_findActor(c->mgr, &s, actorKey);
    bool ok = false;

    __try
    {
        uintptr_t actor = Held(&s);
        uintptr_t ai = actor ? R64(R64(actor + 0x68) + 0x58) : 0;

        if (ai && R64(ai) == g_base + VT_SERVER_AI)
        {
            uint32_t key[3] = { 0xFFFF, hash, 0 };
            key[2] = g_eventIndex(hash);
            g_eventPrepare(key);
            __declspec(align(16)) uint8_t request[0x100] = {};
            g_eventRequest(request, key);
            *(uint32_t*)(request + 0x18) = target;
            *(uint32_t*)(request + 0x1C) = target;
            *(uint32_t*)(request + 0x50) = 0;
            uint32_t result[4] = {};
            typedef void* (__fastcall* RaiseFn)(uintptr_t ai, uint32_t* result, uint8_t* request, uint64_t zero);
            ((RaiseFn)R64(R64(ai) + 0x188))(ai, result, request, 0);
            ok = result[0] == 0 || ((result[0] & 3) == 1 && result[1] == 0);
            Log("  %s event on %08X (target %08X) -> %08X %08X", name, actorKey, target, result[0], result[1]);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    g_scopeRelease(&s);
    return ok;
}

// Her horse called to her (OnCallingVehicle on the horse's AI, with her
// position as the destination, as the game's horse summon 0x2BA71D0 does).
static bool CallHorse(Context* c, Comrade* m)
{
    Scope s = {};
    g_findActor(c->mgr, &s, m->horse);
    bool ok = false;

    __try
    {
        uintptr_t horse = Held(&s);
        uintptr_t ai = horse ? R64(R64(horse + 0x68) + 0x58) : 0;

        if (ai && R64(ai) == g_base + VT_SERVER_AI && m->havePos)
        {
            uint32_t key[3] = { 0xFFFF, EVENT_CALLING_VEHICLE, 0 };
            key[2] = g_eventIndex(EVENT_CALLING_VEHICLE);
            g_eventPrepare(key);
            __declspec(align(16)) uint8_t request[0x100] = {};
            g_eventRequest(request, key);
            *(uint32_t*)(request + 0x18) = m->actor;
            *(uint32_t*)(request + 0x1C) = m->actor;
            *(uint32_t*)(request + 0x20) = 0;
            *(float*)(request + 0x40) = m->x;
            *(float*)(request + 0x44) = m->y;
            *(float*)(request + 0x48) = m->z;
            request[0x4C] = 1;
            *(uint32_t*)(request + 0x50) = 0;
            typedef uint32_t (__fastcall* TagFn)();
            *(uint32_t*)(request + 0x58) = ((TagFn)(g_base + EVENT_TAG_NEW_SUMMONED))();
            uint32_t result[4] = {};
            typedef void* (__fastcall* RaiseFn)(uintptr_t ai, uint32_t* result, uint8_t* request, uint64_t zero);
            ((RaiseFn)R64(R64(ai) + 0x188))(ai, result, request, 0);
            ok = result[0] == 0 || ((result[0] & 3) == 1 && result[1] == 0);
            Log("horses: %s calls her horse %08X -> %08X %08X; horse controller %08X", Name(m), m->horse, result[0], result[1],
                R32(horse + 0x90));
        }
        else
            Log("horses: %s's horse %08X has no server AI to call", Name(m), m->horse);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("horses: calling the horse failed (exception)");
    }

    g_scopeRelease(&s);
    return ok;
}

// The ride interaction started for her (what the game's own request 0xA6F
// does, 0x29E8FA0 -> 0x2B36860): her client AI walks to the horse and mounts.
static bool StartRideInteraction(Context* c, Comrade* m)
{
    Scope s = {};
    g_findActor(c->mgr, &s, m->actor);
    bool ok = false;

    __try
    {
        uintptr_t a = Held(&s);
        uintptr_t comp = a ? R64(R64(a + 0x68) + 0x98) : 0;
        uintptr_t rc = a ? R64(R64(a + 0x68) + 0x48) : 0;
        uintptr_t block = rc ? R64(rc + 0x18) : 0;
        Log("horses: %s controller %08X user flag %u mounted %08X, interaction component %s", Name(m), a ? R32(a + 0x90) : 0,
            a ? R8(a + 0x94) : 0, block ? R32(block + 0x10) : 0, comp && R64(comp) == g_base + VT_SERVER_INTERACTION ? "ok" : "MISSING");

        if (comp && R64(comp) == g_base + VT_SERVER_INTERACTION && R32(a + 0x90))
        {
            // res is 0 on nearly every path: whether it was taken shows in the
            // component (+0x2A step 1, +0x20 the target; 0x2B37219, 0x211E1C0).
            uint32_t res = 0xFFFFFFFF, target = m->horse, interaction = INTERACTION_RIDE_MERCENARY;
            struct { uint8_t f6, pad[3]; uint32_t f7; } extra = { 1, {}, 0 };
            typedef void* (__fastcall* PreemptFn)(uintptr_t comp, uint32_t* res, const uint32_t* target, const uint32_t* interaction,
                                                  uint32_t pivot, uint32_t f5, const void* extra);
            ((PreemptFn)(g_base + INTERACTION_PREEMPT))(comp, &res, &target, &interaction, 0, 0, &extra);
            ok = R8(comp + 0x2A) == 1 && R32(comp + 0x20) == m->horse;
            Log("horses: %s ride interaction on %08X -> %08X; step %u, target %08X, interaction %u, pivot %u, preempting %08X%s", Name(m),
                m->horse, res, R8(comp + 0x2A), R32(comp + 0x20), R16(comp + 0x28), R32(comp + 0x2C), R32(comp + 0xD8),
                ok ? "" : " - NOT taken");
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log("horses: the ride interaction failed (exception)");
    }

    g_scopeRelease(&s);
    return ok;
}

// A ride interaction still pending is let go (comp vt+0x1B0 = 0x2125D30, as
// 0x2B36860 does when one fails).
static void ReleaseRideInteraction(Context* c, Comrade* m)
{
    Scope s = {};
    g_findActor(c->mgr, &s, m->actor);

    __try
    {
        uintptr_t a = Held(&s);
        uintptr_t comp = a ? R64(R64(a + 0x68) + 0x98) : 0;

        if (comp && R64(comp) == g_base + VT_SERVER_INTERACTION && (R8(comp + 0x2A) || R32(comp + 0xD8)))
        {
            uint32_t target = R32(comp + 0x20) ? R32(comp + 0x20) : m->horse, err = 0;
            typedef void (__fastcall* ResetFn)(uintptr_t comp, const uint32_t* target, uint32_t* err, uint64_t zero);
            ((ResetFn)R64(R64(comp) + 0x1B0))(comp, &target, &err, 0);
            Log("horses: %s's ride interaction let go -> %08X", Name(m), err);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    g_scopeRelease(&s);
}

// Her AI told to wait for the ride interaction (the client's own first step),
// then the interaction a moment later.
static void PrepareRide(Context* c, Comrade* m, ULONGLONG now)
{
    RaiseEventOn(c, m->actor, EVENT_PREEMPTION_START, m->horse, "Interaction preemption start");
    m->mountState = 3;
    m->mountAt = now;
}

// Onto her horse: walking to it and mounting (HorseMount), her horse called
// first when it is far; else (or failing) seated at once.
static void Mount(Context* c, Comrade* m, int slot, ULONGLONG now)
{
    if (!g_horseMount)
    {
        if (HorseDistance(c, m) < 0 || HorseDistance(c, m) > HORSE_NEAR_M)
            HorseToHer(c, m);

        QueueSeat(c, m, slot);
        return;
    }

    if (m->horseSpawned)
        LogoutDistance(c, m->horse, true, LEASH_M);

    m->kicked = false;
    m->dismountAt = 0;
    m->mountAt = now;
    float hd = HorseDistance(c, m);
    uint32_t on = RidingOf(c, m->actor);

    // The game still has her on a horse that is not under her (a teleport
    // left it behind): off it first, or no horse takes her.
    if (on && (on != m->horse || hd < 0 || hd > 5.0f))
        RideOff(c, m, on, "a horse left behind");

    if (hd < 0 || hd > HORSE_NEAR_M)
    {
        CallHorse(c, m);
        m->mountState = 1;
        return;
    }

    PrepareRide(c, m, now);
}

static void HorseAfterTeleport(Context* c, Comrade* m, ULONGLONG now)
{
    if (!g_horses || !m->horse || !ActorAlive(c, m->horse))
        return;

    {
        Scope ms = {};
        g_findActor(c->mgr, &ms, m->actor);
        uintptr_t a = Held(&ms);
        float x, y, z;

        if (a && WorldPos(a, &x, &y, &z))
        {
            m->x = x;
            m->y = y;
            m->z = z;
            m->havePos = true;
        }

        g_scopeRelease(&ms);
    }

    float hd = HorseDistance(c, m);

    if (hd >= 0 && hd <= 5.0f)
        return;

    int slot = (int)(m - g_comrades);
    uint32_t on = RidingOf(c, m->actor);

    if (on)
        RideOff(c, m, on, "teleported without it");

    m->mountState = 0;
    HorseToHer(c, m);

    if (g_rideSeen)
    {
        Log("horses: %s's horse brought after her teleport - on again", Name(m));
        QueueSeat(c, m, slot);
    }
}

static void HorsesOnRide(Context* c, bool mounted, bool riding)
{
    if (!g_horses || !g_clientTickOrig)
        return;

    for (int i = 0; i < MAX_COMRADES; ++i)
    {
        Comrade& m = g_comrades[i];

        if (m.phase == PHASE_FREE)
            continue;

        if (!riding && m.horse)
        {
            // Her AI gets off as Damiane's does (the game's OnRideOff_FocusActor);
            // made to only if she is still on after a while.
            m.dismountAt = GetTickCount64() + 4000;

            for (RideReq& r : g_rideReq)
                if (r.rider == m.actor)
                    InterlockedExchange(&r.state, 0);
        }
        else if (mounted && m.horse && ActorAlive(c, m.horse) && m.phase != PHASE_STANDBY && m.phase != PHASE_LOST)
        {
            float d = DistanceTo(c, m.actor), hd = -1;
            Scope hs = {};
            g_findActor(c->mgr, &hs, m.horse);
            uintptr_t h = Held(&hs);
            float x, y, z;

            if (h && m.havePos && WorldPos(h, &x, &y, &z))
                hd = Dist(x, y, z, m.x, m.y, m.z);

            g_scopeRelease(&hs);

            if (d < 0 || d > RIDE_NEAR_M)
                continue;

            m.offSince = 0;
            m.reseats = 0;
            Log("horses: %s gets on her horse %08X (%.0f m from her)", Name(&m), m.horse, hd);
            Mount(c, &m, i, GetTickCount64());
        }
        else if (mounted && m.phase != PHASE_STANDBY && m.phase != PHASE_LOST)
        {
            if (m.horse)
            {
                Log("horses: %s's horse %08X is gone - another one", Name(&m), m.horse);
                m.horse = 0;
                m.horseSpawned = false;
            }

            float d = DistanceTo(c, m.actor);

            if (d < 0 || d > RIDE_NEAR_M)
            {
                Log("horses: %s is %.0f m from you - no horse", Name(&m), d);
                continue;
            }

            uint32_t horse = FindFreeHorse(c, &m);
            bool spawned = false;

            if (!horse && g_horseSpawn)
            {
                horse = SpawnHorse(c, &m);
                spawned = horse != 0;
            }

            if (!horse)
                continue;

            m.horse = horse;
            m.horseSpawned = spawned;
            m.offSince = 0;
            m.reseats = 0;
            Mount(c, &m, i, GetTickCount64());
            Log("horses: %s gets %s horse %08X", Name(&m), spawned ? "a spawned" : "a borrowed", horse);
        }
    }
}

static void HorsesWatch(Context* c, ULONGLONG now)
{
    for (int i = 0; i < MAX_COMRADES; ++i)
    {
        RideReq& r = g_rideReq[i];
        LONG st = r.state;

        if (st == 2 || st == 3)
        {
            Log("horses: seating %08X on %08X %s (tries %d, err %08X)", r.rider, r.horse, st == 2 ? "done" : "FAILED - not found on your side",
                r.tries, r.err);
            InterlockedExchange(&r.state, 0);
        }
    }

    for (Comrade& m : g_comrades)
        if (m.phase != PHASE_FREE && m.horse && m.dismountAt && now >= m.dismountAt)
        {
            m.dismountAt = 0;

            if (!g_rideSeen)
                DropHorse(c, &m, "you got off", false);   // her own horse stays where she gets off
        }

    // HorseMount: her horse arriving, then her walk to it and the mount.
    for (int i = 0; i < MAX_COMRADES; ++i)
    {
        Comrade& m = g_comrades[i];

        if (m.phase == PHASE_FREE || !m.horse || !m.mountState)
            continue;

        if (!g_rideSeen)
        {
            m.mountState = 0;
            continue;
        }

        uint32_t on = 0;
        Scope s = {};
        g_findActor(c->mgr, &s, m.actor);

        __try
        {
            uintptr_t a = Held(&s);
            uintptr_t rc = a ? R64(R64(a + 0x68) + 0x48) : 0;
            uintptr_t block = rc ? R64(rc + 0x18) : 0;
            on = block ? R32(block + 0x10) : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        g_scopeRelease(&s);

        if (on == m.horse && HorseDistance(c, &m) >= 0 && HorseDistance(c, &m) <= 5.0f)
        {
            Log("horses: %s mounted her horse after %.1f s", Name(&m), (now - m.mountAt) / 1000.0);
            m.mountState = 0;
            continue;
        }

        float hd = HorseDistance(c, &m);

        if (m.mountState == 1 && ((hd >= 0 && hd <= HORSE_ARRIVED_M) || now - m.mountAt >= HORSE_CALL_MS))
        {
            Log("horses: %s's horse %s (%.1f m from her)", Name(&m), hd >= 0 && hd <= HORSE_ARRIVED_M ? "came" : "did not come in time", hd);

            if (hd < 0 || hd > HORSE_NEAR_M)
                HorseToHer(c, &m);

            PrepareRide(c, &m, now);
        }
        else if (m.mountState == 3 && now - m.mountAt >= PREEMPT_AFTER_START_MS)
        {
            m.mountState = 2;
            m.mountAt = now;

            if (!StartRideInteraction(c, &m))
            {
                ReleaseRideInteraction(c, &m);
                m.mountState = 0;
                QueueSeat(c, &m, i);
            }
        }
        else if (m.mountState == 2 && now - m.mountAt >= MOUNT_WAIT_MS)
        {
            Log("horses: %s did not mount in %.0f s (%.1f m from her horse) - seated", Name(&m), MOUNT_WAIT_MS / 1000.0, hd);
            ReleaseRideInteraction(c, &m);
            m.mountState = 0;
            QueueSeat(c, &m, i);
        }
    }

    // Off her horse while you ride: seated again (3 times at most per ride).
    for (int i = 0; i < MAX_COMRADES && g_rideSeen; ++i)
    {
        Comrade& m = g_comrades[i];

        if (m.phase != PHASE_FOLLOWING || !m.horse || g_rideReq[i].state || m.dismountAt || m.mountState)
            continue;

        uint32_t on = 0;
        Scope s = {};
        g_findActor(c->mgr, &s, m.actor);

        __try
        {
            uintptr_t a = Held(&s);
            uintptr_t rc = a ? R64(R64(a + 0x68) + 0x48) : 0;
            uintptr_t block = rc ? R64(rc + 0x18) : 0;
            on = block ? R32(block + 0x10) : 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        g_scopeRelease(&s);

        if (on)
        {
            m.offSince = 0;

            // Seated: her AI only rides after you once it restarts (as after
            // her own catch-up teleport) - restarted, then Follow.
            if (!m.kicked && on == m.horse)
            {
                m.kicked = true;
                Resync(c, m.actor, "on her horse");
                RaiseLater(m.actor, EVENT_FOLLOW, "Follow (on her horse)", now + 1000);
            }
        }
        else if (!m.offSince)
            m.offSince = now;
        else if (now - m.offSince >= 3000 && m.reseats < 3 && ActorAlive(c, m.horse))
        {
            ++m.reseats;
            m.offSince = 0;
            float hd = -1;
            Scope hs = {};
            g_findActor(c->mgr, &hs, m.horse);
            uintptr_t h = Held(&hs);
            float x, y, z;

            if (h && m.havePos && WorldPos(h, &x, &y, &z))
                hd = Dist(x, y, z, m.x, m.y, m.z);

            g_scopeRelease(&hs);

            Log("horses: %s got off her horse while you ride - on again (%d, %.0f m from it)", Name(&m), m.reseats, hd);
            Mount(c, &m, i, now);
        }
    }

    if (now - g_lastHorseLog < (g_detailedLog ? STATUS_MS : QUIET_STATUS_MS))
        return;

    g_lastHorseLog = now;

    for (Comrade& m : g_comrades)
    {
        if (m.phase == PHASE_FREE || !m.horse)
            continue;

        Scope s = {};
        g_findActor(c->mgr, &s, m.actor);

        __try
        {
            uintptr_t a = Held(&s);
            uintptr_t rc = a ? R64(R64(a + 0x68) + 0x48) : 0;
            uintptr_t block = rc ? R64(rc + 0x18) : 0;
            Log("horses: %s rides %08X (seat %d; horse %08X), %.1f m from you", Name(&m), block ? R32(block + 0x10) : 0,
                block ? (int)(int8_t)R8(block + 0x14) : -1, m.horse, DistanceTo(c, m.actor));

            Scope hs = {};
            g_findActor(c->mgr, &hs, m.horse);
            uintptr_t h = Held(&hs);
            uintptr_t hcc = h ? R64(R64(h + 0x68) + 0x20) : 0;
            uintptr_t vc = h ? R64(R64(h + 0x68) + 0x48) : 0;
            uintptr_t hai = h ? R64(R64(h + 0x68) + 0x58) : 0;

            if (!h)
                Log("  horse %08X: GONE", m.horse);
            else
                Log("  horse %08X: driver %08X, riders %u, flags %08X/%08X, spawn type %u, logout distance %.0f, %.1f m from you,"
                    " controller %08X, AI sequence %llX", m.horse, vc ? R32(vc + 0x48) : 0, vc ? R8(vc + 0xC8) : 0,
                    hcc ? R32(hcc + 0x390) : 0, hcc ? R32(hcc + 0x394) : 0, hcc ? R8(hcc + 0x2D0) : 0, hcc ? RF(hcc + 0x468) : 0,
                    DistanceTo(c, m.horse), R32(h + 0x90), hai ? (unsigned long long)R64(hai + 0x18) : 0ull);

            g_scopeRelease(&hs);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }

        g_scopeRelease(&s);
    }
}

// The client thread: the minimap update of your own character runs the ride
// requests (the client actors looked up by key, then the game's ride-on).
static uintptr_t ClientActorOf(uint32_t key, Scope* s)
{
    uintptr_t mgr = R64(R64(g_base + CLIENT_WORLD) + 0x10);

    if (!mgr || R64(mgr) != g_base + VT_CLIENT_ACTORS || R64(R64(mgr) + 0x28) != g_base + CLIENT_FIND_ACTOR)
        return 0;

    typedef void* (__fastcall* FindFn)(uintptr_t mgr, Scope* out, uint32_t key);
    ((FindFn)(g_base + CLIENT_FIND_ACTOR))(mgr, s, key);
    return Held(s);
}

static void __fastcall ClientTickDetour(uintptr_t comp)
{
    bool any = false;

    for (const RideReq& r : g_rideReq)
        any |= r.state == 1;

    if (any)
    {
        __try
        {
            uintptr_t you = R64(R64(R64(g_base + CLIENT_ROOT) + 0x30) + 0x50);

            if (you && R64(comp + 8) == you)
                for (RideReq& r : g_rideReq)
                {
                    if (r.state != 1)
                        continue;

                    Scope a = {}, b = {};
                    uintptr_t rider = ClientActorOf(r.rider, &a), horse = ClientActorOf(r.horse, &b);

                    if (rider && horse)
                    {
                        typedef void (__fastcall* RideOnFn)(uint64_t unused, uintptr_t sender, uintptr_t rider, uintptr_t vehicle);
                        ((RideOnFn)(g_base + RIDE_ON_CLIENT))(0, you, rider, horse);
                        InterlockedExchange(&r.state, 2);
                    }
                    else if (++r.tries > 600)
                        InterlockedExchange(&r.state, 3);

                    g_scopeRelease(&b);
                    g_scopeRelease(&a);
                }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
    }

    if (g_mapIconRefresh)
        MapRefreshBits(comp);

    // A comrade's map icon: its position is frozen (comp+0x35 bit 2, 0x9E7C87)
    // unless bit 3 asks for an update - asked for on every tick.
    if (g_mapIcon)
    {
        uintptr_t actor = 0;

        if (MapSlot(comp, &actor) >= 0)
        {
            __try
            {
                if ((R8(comp + 0x35) & 0x0C) == 0x04)
                    *(volatile uint8_t*)(comp + 0x35) |= 0x08;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
            }
        }
    }

    g_clientTickOrig(comp);
}

static void InstallHorses()
{
    if (!Matches(ACTOR_SNAPSHOT, "48895C2420 55 56 57 4154 4155 4156 4157 4883EC40 4C8BFA") ||
        !Matches(RIDE_ON_CLIENT, "48895C2408 55 56 57 4883EC40 498B4168 498BF1 488BEA C644242002"))
    {
        Log("horses: the game code is not as expected - off");
        return;
    }

    if (!SwapSlot(SLOT_MINIMAP_UPDATE, MINIMAP_UPDATE, (void*)&ClientTickDetour, (void**)&g_clientTickOrig, "horses"))
        return;

    Log("horses: on (spawn %s)", g_horseSpawn ? "on" : "off");
}

static void HookProbes()
{
    struct Hook { uintptr_t rva; void* detour; void** original; const char* name; bool* installed; };
    const Hook hooks[] = {
        { AI_TELEPORT_REQUEST, (void*)&ProbeRequest, (void**)&g_probeRequest, "teleport request", NULL },
        { AI_TELEPORT_CHECK, (void*)&ProbeCheck, (void**)&g_probeCheck, "teleport check", &g_checkProbe },
        { LOGOUT_DISTANCE_TEST, (void*)&ProbeDistance, (void**)&g_probeDistance, "logout distance test", NULL },
        { AI_TELEPORT, (void*)&ProbeTeleport, (void**)&g_probeTeleport, "AI teleport", NULL },
    };

    for (const Hook& h : hooks)
    {
        void* target = (void*)(g_base + h.rva);
        MH_STATUS st;

        if ((st = MH_CreateHook(target, h.detour, h.original)) != MH_OK || (st = MH_EnableHook(target)) != MH_OK)
            Log("probe %s: could not hook (%d)", h.name, (int)st);
        else if (h.installed)
            *h.installed = true;
    }
}

bool GameInit()
{
    g_base = (uintptr_t)GetModuleHandleW(NULL);
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(g_base + ((IMAGE_DOS_HEADER*)g_base)->e_lfanew);

    if (nt->FileHeader.TimeDateStamp != KNOWN_TIMESTAMP || nt->OptionalHeader.SizeOfImage != KNOWN_IMAGE_SIZE)
    {
        Log("game build %08lX / %08lX is not the known one - nothing is hooked", nt->FileHeader.TimeDateStamp,
            nt->OptionalHeader.SizeOfImage);
        return false;
    }

    for (const Prologue& p : PROLOGUES)
        if (!Matches(p.rva, p.hex))
        {
            Log("%s does not look as expected - nothing is hooked", p.name);
            return false;
        }

    for (const Call& k : CALLS)
        if (CallTarget(k.site) != g_base + k.target)
        {
            Log("%s is not called where expected - nothing is hooked", k.name);
            return false;
        }

    // findActor: the server actor manager's vtable slot; the AI's teleport:
    // the server AI's.
    if (R64(g_base + VT_SERVER_ACTORS + 0x28) != g_base + FIND_ACTOR || R64(g_base + VT_SERVER_AI + 0x208) != g_base + AI_TELEPORT)
    {
        Log("findActor / the AI teleport not where expected - nothing is hooked");
        return false;
    }

    g_accompany = (AccompanyFn)(g_base + ACCOMPANY);
    g_charInfo = (CharInfoFn)(g_base + CHARACTER_BY_INDEX);
    g_findActor = (FindActorFn)(g_base + FIND_ACTOR);
    g_scopeRelease = (ScopeReleaseFn)(g_base + SCOPE_RELEASE);
    g_position = (PositionFn)(g_base + POSITION);
    g_setLogoutDistance = (SetLogoutDistanceFn)(g_base + SET_LOGOUT_DISTANCE);
    g_disband = (DisbandFn)(g_base + DISBAND);
    g_aiTeleport = (AiTeleportFn)(g_base + AI_TELEPORT);
    g_setAccompanyType = (SetAccompanyTypeFn)(g_base + SET_ACCOMPANY_TYPE);
    g_ack = (AckFn)(g_base + ACCOMPANY_TYPE_ACK);
    g_readLock = (ReadLockFn)(g_base + READ_LOCK);
    g_eventIndex = (EventIndexFn)(g_base + AI_EVENT_INDEX);
    g_eventPrepare = (EventPrepareFn)(g_base + AI_EVENT_PREPARE);
    g_eventRequest = (EventRequestFn)(g_base + AI_EVENT_REQUEST);
    g_fieldScope = (FieldScopeFn)(g_base + FIELD_SCOPE);
    g_teleportInit = (TeleportInitFn)(g_base + TELEPORT_INIT);
    g_safeSpot = (SafeSpotFn)(g_base + SAFE_SPOT);
    g_teleport = (TeleportFn)(g_base + TELEPORT);
    g_removeAccompany = (RemoveAccompanyFn)(g_base + REMOVE_ACCOMPANY);
    g_interactionByIndex = (InteractionByIndexFn)(g_base + INTERACTION_BY_INDEX);
    g_resync = (ResyncFn)(g_base + RESYNC);

    MH_STATUS st = MH_Initialize();

    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED)
    {
        Log("MinHook: %d", (int)st);
        return false;
    }

    void* target = (void*)(g_base + SERVER_DISPATCH);

    if ((st = MH_CreateHook(target, (void*)&HookDispatch, (void**)&g_dispatch)) != MH_OK || (st = MH_EnableHook(target)) != MH_OK)
    {
        Log("could not hook the server dispatch (%d)", (int)st);
        return false;
    }

    target = (void*)(g_base + TRY_PREEMPT);

    if ((st = MH_CreateHook(target, (void*)&HookTryPreempt, (void**)&g_tryPreempt)) != MH_OK || (st = MH_EnableHook(target)) != MH_OK)
        Log("could not hook TryPreempt (%d) - Recruit and Dismiss will not work", (int)st);

    HookProbes();
    g_teleportEffect = IniOption("TeleportEffect", 1) != 0;
    g_hideRoutine = IniOption("HideRoutine", 1) != 0;
    g_dropRoutine = IniOption("DropRoutine", 0) != 0;
    g_stayBehind = IniOption("StayBehind", 1) != 0;
    g_mapIcon = IniOption("MapIcon", 1) != 0;
    g_mapIconOffset = IniOption("MapIconStyle", 0) == 1 ? MAP_TEX_CHARACTER : MAP_TEX_PARTY;
    g_mapIconRefresh = IniOption("MapIconRefresh", 0) != 0;
    g_horses = IniOption("Horses", 1) != 0;
    g_horseSpawn = IniOption("HorseSpawn", 1) != 0;
    g_horseMount = IniOption("HorseMount", 0) != 0;   // the walk to the horse is not working yet
    g_detailedLog = IniOption("DetailedLog", 0) != 0;
    Log("experiments: catch-up teleport %s, hide the routine %s, drop it at once %s, stay behind %s, map icon %s (%s), map refresh %s",
        g_teleportEffect ? "on" : "off", g_hideRoutine ? "on" : "off", g_dropRoutine ? "on" : "off", g_stayBehind ? "on" : "off",
        g_mapIcon ? "on" : "off", g_mapIconOffset == MAP_TEX_PARTY ? "party" : "character", g_mapIconRefresh ? "on" : "off");

    // The safe-spot slot (Dismiss keeps her where she is) - a table entry, as
    // only this one pointer leads to 0x2B98E20.
    if (!SwapSlot(SLOT_SAFE_SPOT, CLAN_TELEPORT, (void*)&SafeSpotDetour, (void**)&g_clanSafeSpot, "safe spot"))
    {
        g_clanSafeSpot = NULL;
        Log("a dismissed comrade may be moved away");
    }

    if (!SwapSlot(SLOT_DISBAND_ACK, DISBAND_ACK, (void*)&DisbandAckDetour, (void**)&g_disbandAck, "disband notice"))
    {
        g_disbandAck = NULL;
        Log("a dismissed comrade may vanish");
    }

    if (Matches(REMOVE_ACTOR_ACK, "48895C2408 4889542410 55 56 57 4154 4155 4156 4157 488DAC24C0EFFFFF B850110000"))
    {
        void* t = (void*)(g_base + REMOVE_ACTOR_ACK);

        if ((st = MH_CreateHook(t, (void*)&ProbeRemoveActor, (void**)&g_probeRemoveActor)) != MH_OK || (st = MH_EnableHook(t)) != MH_OK)
            Log("probe remove actor: could not hook (%d)", (int)st);
    }

    if (g_hideRoutine)
        InstallRoutineHide();

    if (g_mapIcon)
        InstallMapIcon();

    if (g_horses)
        InstallHorses();

    target = (void*)(g_base + CAN_SWITCH);

    if ((st = MH_CreateHook(target, (void*)&SwitchDetour, (void**)&g_canSwitch)) != MH_OK || (st = MH_EnableHook(target)) != MH_OK)
        Log("probe switch check: could not hook (%d)", (int)st);

    Log("ready: Recruit and Dismiss are talk options; Bring back brings your companions to you");
    return true;
}
