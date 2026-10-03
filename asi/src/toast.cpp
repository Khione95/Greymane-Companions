#include "toast.h"
#include "overlay.h"

#include <windows.h>
#include <string>

// ---------------------------------------------------------------------------
// A notice at the top right for a few seconds (Recruit, a full party), drawn
// as Seasons of Pywel's season notice: a dark panel fading in from the left,
// thin gold lines, a gold title and a light line of text.
// ---------------------------------------------------------------------------

static SRWLOCK g_lock = SRWLOCK_INIT;
static std::wstring g_title, g_text;
static DWORD g_at = 0;
static const DWORD TOAST_MS = 6000;

struct Style
{
    bool ready;
    IDWriteTextFormat* title;
    IDWriteTextFormat* caption;
    ID2D1SolidColorBrush* gold;
    ID2D1SolidColorBrush* text;
    ID2D1SolidColorBrush* line;
    ID2D1LinearGradientBrush* panel;
    float scale;
};

static Style g_style;

template <class T>
static void Release(T*& p)
{
    if (p)
        p->Release();

    p = NULL;
}

static IDWriteTextFormat* Font(IDWriteFactory* write, float size)
{
    IDWriteTextFormat* f = NULL;
    write->CreateTextFormat(L"Georgia", NULL, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size,
                            L"en-us", &f);

    if (f)
    {
        f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    }

    return f;
}

static void MakeStyle(const OverlayDrawContext& ctx)
{
    float s = ctx.height / 1080.0f;
    ID2D1DeviceContext* dc = ctx.dc;
    Style& st = g_style;

    st.scale = s;
    st.title = Font(ctx.write, 26 * s);
    st.caption = Font(ctx.write, 17 * s);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.93f, 0.76f, 0.45f), &st.gold);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.93f, 0.91f, 0.87f), &st.text);
    dc->CreateSolidColorBrush(D2D1::ColorF(0.55f, 0.47f, 0.32f, 0.8f), &st.line);

    D2D1_GRADIENT_STOP stops[] = {
        { 0.0f, D2D1::ColorF(0.06f, 0.055f, 0.05f, 0.0f) },
        { 0.12f, D2D1::ColorF(0.06f, 0.055f, 0.05f, 0.82f) },
        { 1.0f, D2D1::ColorF(0.06f, 0.055f, 0.05f, 0.9f) } };
    ID2D1GradientStopCollection* collection = NULL;
    dc->CreateGradientStopCollection(stops, 3, &collection);

    if (collection)
    {
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(1, 0)), collection,
                                      &st.panel);
        collection->Release();
    }

    st.ready = st.title && st.caption && st.gold && st.text && st.line && st.panel;
}

static void ForgetDrawing()
{
    Style& st = g_style;
    Release(st.title);
    Release(st.caption);
    Release(st.gold);
    Release(st.text);
    Release(st.line);
    Release(st.panel);
    st = {};
}

static void Text(ID2D1DeviceContext* dc, const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, ID2D1Brush* b)
{
    dc->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static bool Wanted()
{
    AcquireSRWLockShared(&g_lock);
    bool live = !g_title.empty() && GetTickCount() - g_at <= TOAST_MS;
    ReleaseSRWLockShared(&g_lock);
    return live;
}

static void Draw(const OverlayDrawContext& ctx)
{
    static unsigned generation = 0;

    if (ctx.generation != generation)
    {
        if (generation)
            ForgetDrawing();

        generation = ctx.generation;
    }

    if (!g_style.ready)
    {
        MakeStyle(ctx);

        if (!g_style.ready)
            return;
    }

    AcquireSRWLockShared(&g_lock);
    std::wstring title = g_title, text = g_text;
    DWORD at = g_at;
    ReleaseSRWLockShared(&g_lock);

    if (title.empty() || GetTickCount() - at > TOAST_MS)
        return;

    // Below Seasons of Pywel's notice (y 50..170), so both can show at once.
    ID2D1DeviceContext* dc = ctx.dc;
    Style& st = g_style;
    float s = st.scale;
    float x0 = ctx.width - 640 * s, x1 = ctx.width - 24 * s, y0 = 190 * s, y1 = y0 + 120 * s;
    st.panel->SetStartPoint(D2D1::Point2F(x0 - 160 * s, 0));
    st.panel->SetEndPoint(D2D1::Point2F(x1, 0));
    dc->FillRectangle(D2D1::RectF(x0 - 160 * s, y0, x1, y1), st.panel);
    dc->DrawLine(D2D1::Point2F(x0 - 40 * s, y0), D2D1::Point2F(x1, y0), st.line, 1.2f * s);
    dc->DrawLine(D2D1::Point2F(x0 - 40 * s, y1), D2D1::Point2F(x1, y1), st.line, 1.2f * s);
    Text(dc, title, st.title, D2D1::RectF(x0, y0 + 10 * s, x1, y0 + 58 * s), st.gold);
    Text(dc, text, st.caption, D2D1::RectF(x0, y0 + 62 * s, x1, y1 - 12 * s), st.text);
}

void ToastInit()
{
    OverlaySetDraw(Draw, Wanted);
}

void Toast(const char* title, const char* text)
{
    wchar_t t[256], x[512];
    MultiByteToWideChar(CP_UTF8, 0, title, -1, t, _countof(t));
    MultiByteToWideChar(CP_UTF8, 0, text, -1, x, _countof(x));
    AcquireSRWLockExclusive(&g_lock);
    g_title = t;
    g_text = x;
    g_at = GetTickCount();
    ReleaseSRWLockExclusive(&g_lock);
}
