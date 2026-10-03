#pragma once

#include <stdint.h>

// A comrade's health, read on the server thread (Dismiss is refused while she
// is down or dead). The party's health on screen is Party HUD's.

enum HealthState : uint8_t { HEALTH_OK, HEALTH_DOWN, HEALTH_DEAD, HEALTH_AWAY, HEALTH_UNKNOWN };

struct Health
{
    float hp, maxHp;        // in the game's own units (the stored value / 1000)
    uint8_t state;          // HealthState
};
