#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>

// Draws into the game's own frames (from Seasons of Pywel's and Character
// Creator's overlays). The game's DirectX 12 back buffer is wrapped for
// Direct2D right before each frame is presented; HDR screens get the drawing
// converted. It takes no keys: the notices only show.

struct OverlayDrawContext
{
    ID2D1DeviceContext* dc;
    IDWriteFactory* write;
    float width;
    float height;
    unsigned generation;    // changes when the drawing objects were made anew:
                            // brushes, fonts and bitmaps made before are invalid
};

typedef void (*OverlayDrawFn)(const OverlayDrawContext& ctx);

// Whether there is anything to draw now (nothing is wrapped or drawn if not).
typedef bool (*OverlayWantedFn)();

// Call early (a second after the plugin starts): watches for the game
// creating its swap chain.
bool OverlayEarlyInit();

void OverlaySetDraw(OverlayDrawFn draw, OverlayWantedFn wanted);
void OverlaySetDrawing(bool drawing);
bool OverlayDrawing();
