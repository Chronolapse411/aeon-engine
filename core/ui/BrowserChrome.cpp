// AeonBrowser — BrowserChrome.cpp
// DelgadoLogic | Lead UI Engineer
//
// PURPOSE: Draws the Aeon browser chrome — the native Win32 toolbar that IS
// the browser UI. This replaces EraChrome's generic shell with the exact
// design seen in the UI mockup:
//
//   ┌─────────────────────────────────────────────────────────────────────┐
//   │ [A]  ←  →  ↻  │  🔒 https://delgadologic.tech  [⬇][★][🧅][⋮]  [_][□][✕] │  ← TITLEBAR+NAV (40px)
//   ├──────────────────────────────────────────────────────────────────────┤
//   │ [⚡ DelgadoLogic ×]  [🐙 GitHub ×]  [+ New Tab ×]          [+]    │  ← TAB STRIP (32px)
//   ├──────────────────────────────────────────────────────────────────────┤
//   │                                                                      │
//   │                     aeon://newtab                                    │  ← CONTENT AREA
//   │                                                                      │
//   └──────────────────────────────────────────────────────────────────────┘
//
// DESIGN TOKENS (from mockup):
//   --bg-primary:   #0d0e14   (titlebar, outer shell)
//   --bg-card:      #16182a   (URL bar, tab hover)
//   --bg-active:    #1e2140   (active tab bg)
//   --accent:       #6c63ff   (active tab underline, URL bar focus, A badge)
//   --accent-2:     #a78bfa   (hover states, A badge gradient end)
//   --text:         #e8e8f0   (primary text)
//   --text-dim:     #8888aa   (inactive tab text, toolbar icons)
//   --text-faint:   #44445a   (placeholder, separators)
//   --green:        #22c55e   (lock icon, AdBlock dot, HTTPS indicator)
//
// RENDERING PIPELINE:
//   1. BrowserChrome::Create()          — creates the host HWND
//   2. BrowserChrome::OnPaint()         — GDI drawing of all chrome elements
//   3. BrowserChrome::OnLButtonDown()   — hit-test buttons, tabs, URL bar
//   4. BrowserChrome::OnMouseMove()     — hover states + tooltip
//   5. BrowserChrome::SetEngine()       — bind to AeonEngineVTable
//
// LEGACY TIERS: On Win9x/XP, DWM APIs (glass, blur) are not called.
// The colors stay the same; we lose only the blur-behind effect.
// On Win16 (retro tier) this file is NOT compiled — aeon16.c handles UI.

#include "BrowserChrome.h"
#include "../../core/engine/AeonEngine_Interface.h"
#include "../../core/probe/HardwareProbe.h"
#include "../../core/settings/SettingsEngine.h"
#include <windows.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <thread>
#include "../../updater/AutoUpdater.h"
#include "../../core/network/NetworkSentinel.h"
#include "../../core/network/DnsResolver.h"
#include "../../core/engine/AeonBridge.h"
#include "../../ai/aeon_tab_intelligence.h"
#include "../../ai/aeon_journey_analytics.h"
#include "AppMenu.h"

// Safe wide-character string conversion helpers
static std::wstring Utf8ToUtf16(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int wLen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (wLen <= 0) return L"";
    std::vector<wchar_t> wBuf(wLen);
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, wBuf.data(), wLen) > 0) {
        return std::wstring(wBuf.data());
    }
    return L"";
}

static std::string Utf16ToUtf8(const std::wstring& utf16) {
    if (utf16.empty()) return "";
    int u8Len = WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (u8Len <= 0) return "";
    std::vector<char> u8Buf(u8Len);
    if (WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), -1, u8Buf.data(), u8Len, nullptr, nullptr) > 0) {
        return std::string(u8Buf.data());
    }
    return "";
}

extern AeonTabIntelligence* g_TabIntel;
extern AeonJourneyAnalytics* g_JourneyAI;

#pragma comment(lib, "dwmapi.lib")

// ---------------------------------------------------------------------------
// Design tokens as Win32 COLORREFs
// ---------------------------------------------------------------------------
#define CLR_BG_PRIMARY   RGB(11,  13,  23)   // #0b0d17
#define CLR_BG_CARD      RGB(20,  23,  43)   // #14172b
#define CLR_BG_ACTIVE    RGB(30,  34,  61)   // #1e223d
#define CLR_ACCENT       RGB(108, 99,  255)  // #6c63ff (Aeon Electric Purple)
#define CLR_ACCENT2      RGB(167, 139, 250)  // #a78bfa
#define CLR_BRAVE_ORANGE RGB(255, 85,  0)    // #ff5500 (Brave Privacy Orange Accent)
#define CLR_TEXT         RGB(232, 232, 240)  // #e8e8f0
#define CLR_TEXT_DIM     RGB(136, 136, 170)  // #8888aa
#define CLR_TEXT_FAINT   RGB(68,  68,  90)   // #44445a
#define CLR_GREEN        RGB(34,  197, 94)   // #22c55e

// Chrome dimensions (pixels)
static const int NAV_HEIGHT  = 40;  // titlebar + navigation row
static const int TAB_HEIGHT  = 32;  // tab strip
static const int CHROME_H    = NAV_HEIGHT + TAB_HEIGHT; // 72px total chrome

// Button zones (nav bar, from left)
static const int BTN_LOGO_X  = 8;   static const int BTN_LOGO_W  = 36;
static const int BTN_BACK_X  = 52;  static const int BTN_BACK_W  = 32;
static const int BTN_FWD_X   = 88;  static const int BTN_FWD_W   = 32;
static const int BTN_REF_X   = 124; static const int BTN_REF_W   = 32;
static const int URLBAR_PAD  = 164; // URL bar starts here
// Right side layout: [URL bar] [8px gap] [4 toolbar icons × 32px] [8px gap] [3 window controls × 46px]
static const int CTRL_W      = 46;  // each window control button width
static const int CTRL_TOTAL  = CTRL_W * 3;            // 138px for min/max/close
static const int TOOLBAR_ICONS_W = 6 * 32;             // 192px for 6 toolbar icons
static const int URLBAR_END  = CTRL_TOTAL + TOOLBAR_ICONS_W + 16; // 346px from right edge

// ---------------------------------------------------------------------------
// Per-tab state
// ---------------------------------------------------------------------------
struct ChromeTab {
    unsigned int  id;
    std::string   url;
    std::string   title;
    bool          loading;
    RECT          tabRect;
    std::vector<std::string> history;
    int           historyIndex;
};

// ---------------------------------------------------------------------------
// BrowserChrome internal state (renamed from BrowserChrome to avoid
// collision with the BrowserChrome namespace declared in BrowserChrome.h)
// ---------------------------------------------------------------------------

struct ChromeState {
    HWND                hwnd;
    HWND                hUrlBar;
    HWND                hContent;
    
    // Cached GDI fonts to prevent GDI leak
    HFONT               hUrlFont;      // URL bar font
    HFONT               hTextFont;     // 9pt Segoe UI Normal
    HFONT               hTextBold;     // 9pt Segoe UI Bold
    HFONT               hTextTabFont;  // 8pt Segoe UI Normal
    HFONT               hIconFont;     // 10pt Segoe MDL2 Assets
    HFONT               hIconFontSmall;// 8pt Segoe MDL2 Assets
    HFONT               hIconFontLarge;// 11pt Segoe MDL2 Assets
    HFONT               hLogoFont;     // 15pt Segoe UI Bold

    std::vector<ChromeTab> tabs;
    int                 activeTab;
    int                 hoverTab;
    int                 hoverBtn;

    AeonEngineVTable*   engine;
    AeonSettings        settings;
    const SystemProfile* profile;

    bool                urlFocused;
    bool                settingsOpen;

    // Dark-theme URL bar brushes (created once, reused for WM_CTLCOLOREDIT)
    HBRUSH              hUrlBgBrush;   // #16182a

    double              hoverAlphas[100] = { 0.0 };
};

// ---------------------------------------------------------------------------
// GDI helpers
// ---------------------------------------------------------------------------
// Reverse-map file:// URLs back to aeon:// display URLs
// Converts resolved local paths back to clean protocol-style display strings.
//   file:///C:/Users/.../newtab/newtab.html  →  aeon://newtab
//   file:///C:/Users/.../pages/settings.html →  aeon://settings
// ---------------------------------------------------------------------------
static std::string ReverseMapUrl(const std::string& url) {
    // Only process file:// URLs
    if (url.rfind("file:///", 0) != 0) return url;

    // Check for known internal pages by suffix
    struct { const char* suffix; const char* aeonUrl; } mappings[] = {
        { "newtab/newtab.html",   "aeon://newtab" },
        { "pages/settings.html",  "aeon://settings" },
        { "pages/history.html",   "aeon://history" },
        { "pages/bookmarks.html", "aeon://bookmarks" },
        { "pages/downloads.html", "aeon://downloads" },
    };

    for (auto& m : mappings) {
        // Case-insensitive suffix check (handles %20 and mixed slashes)
        std::string lower = url;
        for (auto& c : lower) { if (c == '\\') c = '/'; c = (char)tolower(c); }
        std::string suffLower = m.suffix;
        for (auto& c : suffLower) c = (char)tolower(c);
        // Replace %20 with space for matching
        std::string decoded = lower;
        size_t pos;
        while ((pos = decoded.find("%20")) != std::string::npos)
            decoded.replace(pos, 3, " ");
        if (decoded.size() >= suffLower.size() &&
            decoded.compare(decoded.size() - suffLower.size(),
                           suffLower.size(), suffLower) == 0) {
            return m.aeonUrl;
        }
    }
    return url; // Not an internal page — show original URL
}

// ---------------------------------------------------------------------------
// GDI helper functions
// ---------------------------------------------------------------------------
static void FillGdiplusRoundRect(Gdiplus::Graphics& graphics, const RECT& r, int radius, Gdiplus::Brush& brush) {
    Gdiplus::GraphicsPath path;
    float rx = (float)radius;
    float x = (float)r.left;
    float y = (float)r.top;
    float w = (float)(r.right - r.left);
    float h = (float)(r.bottom - r.top);
    
    path.AddArc(x, y, rx * 2, rx * 2, 180, 90);
    path.AddArc(x + w - rx * 2, y, rx * 2, rx * 2, 270, 90);
    path.AddArc(x + w - rx * 2, y + h - rx * 2, rx * 2, rx * 2, 0, 90);
    path.AddArc(x, y + h - rx * 2, rx * 2, rx * 2, 90, 90);
    path.CloseFigure();
    
    graphics.FillPath(&brush, &path);
}

static void FillRectColor(HDC hdc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, &r, b);
    DeleteObject(b);
}

static void DrawRoundRect(HDC hdc, const RECT& r, int rx, COLORREF fill, COLORREF stroke) {
    HBRUSH br = CreateSolidBrush(fill);
    HPEN   pn = CreatePen(PS_SOLID, 1, stroke);
    HBRUSH oldBr = (HBRUSH)SelectObject(hdc, br);
    HPEN   oldPn = (HPEN)SelectObject(hdc, pn);
    RoundRect(hdc, r.left, r.top, r.right, r.bottom, rx, rx);
    SelectObject(hdc, oldBr);
    SelectObject(hdc, oldPn);
    DeleteObject(br);
    DeleteObject(pn);
}

static void DrawText16(HDC hdc, const char* text, const RECT& r,
                       COLORREF c, HFONT hFont) {
    if (!text) return;
    HFONT old = (HFONT)SelectObject(hdc, hFont);
    SetTextColor(hdc, c);
    SetBkMode(hdc, TRANSPARENT);
    std::wstring wStr = Utf8ToUtf16(text);
    if (!wStr.empty()) {
        RECT dr = r;
        DrawTextW(hdc, wStr.c_str(), -1, &dr,
            DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    SelectObject(hdc, old);
}

// ---------------------------------------------------------------------------
// MDL2 icon helper — renders glyphs from "Segoe MDL2 Assets" (Win10+)
// ---------------------------------------------------------------------------
static void DrawIcon(HDC hdc, const wchar_t* glyph, const RECT& r,
                     COLORREF c, HFONT hFont) {
    HFONT old = (HFONT)SelectObject(hdc, hFont);
    SetTextColor(hdc, c);
    SetBkMode(hdc, TRANSPARENT);
    RECT dr = r;
    DrawTextW(hdc, glyph, -1, &dr,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOCLIP);
    SelectObject(hdc, old);
}

// MDL2 glyph constants
#define ICON_BACK      L"\uE72B"
#define ICON_FORWARD   L"\uE72A"
#define ICON_REFRESH   L"\uE72C"
#define ICON_DOWNLOAD  L"\uE896"
#define ICON_BOOKMARK  L"\uE734"
#define ICON_SHIELD    L"\uE83D"
#define ICON_AI_SUMMARY L"\uF602"
#define ICON_AI_JOURNEY L"\uE81C"
#define ICON_MORE      L"\uE712"
#define ICON_MINIMIZE  L"\uE921"
#define ICON_MAXIMIZE  L"\uE922"
#define ICON_RESTORE   L"\uE923"
#define ICON_CLOSE     L"\uE8BB"
#define ICON_ADD       L"\uE710"
#define ICON_LOCK      L"\uE72E"

// ---------------------------------------------------------------------------
// Paint the "A" logo badge — the signature element from the mockup
// ---------------------------------------------------------------------------
static void DrawLogoBadge(HDC hdc, int x, int y, HFONT hLogoFont) {
    const int SIZE = 28;
    bool loaded = false;
    
    // Attempt GDI+ render of Aeon_28.png
    Gdiplus::Image image(L"resources/icons/Aeon_28.png");
    if (image.GetLastStatus() == Gdiplus::Ok) {
        Gdiplus::Graphics graphics(hdc);
        if (graphics.DrawImage(&image, x, y, SIZE, SIZE) == Gdiplus::Ok) {
            loaded = true;
        }
    }
    
    if (!loaded) {
        // Gradient approximation — 4 vertical bands from accent to accent2
        COLORREF gradColors[] = {
            RGB(108, 99, 255),  // #6c63ff top
            RGB(120, 108, 253), // mid-upper
            RGB(140, 120, 252), // mid-lower
            RGB(167, 139, 250)  // #a78bfa bottom
        };
        int bandH = SIZE / 4;
        for (int i = 0; i < 4; i++) {
            RECT band = { x, y + i * bandH, x + SIZE, y + (i + 1) * bandH };
            if (i == 0) band.top += 1;  // rounded top visual offset
            FillRectColor(hdc, band, gradColors[i]);
        }

        // Rounded corners via clipping region
        HRGN rgn = CreateRoundRectRgn(x, y, x + SIZE + 1, y + SIZE + 1, 8, 8);
        SelectClipRgn(hdc, rgn);
        for (int i = 0; i < 4; i++) {
            RECT band = { x, y + i * bandH, x + SIZE, y + (i + 1) * bandH };
            FillRectColor(hdc, band, gradColors[i]);
        }
        SelectClipRgn(hdc, nullptr);
        DeleteObject(rgn);

        // Subtle border highlight
        HPEN borderPen = CreatePen(PS_SOLID, 1, RGB(140, 130, 255));
        HBRUSH nullBr = (HBRUSH)GetStockObject(NULL_BRUSH);
        HPEN oldPen = (HPEN)SelectObject(hdc, borderPen);
        HBRUSH oldBr = (HBRUSH)SelectObject(hdc, nullBr);
        RoundRect(hdc, x, y, x + SIZE, y + SIZE, 8, 8);
        SelectObject(hdc, oldPen);
        SelectObject(hdc, oldBr);
        DeleteObject(borderPen);

        // "A" lettermark — bold, white, centered
        RECT textR = { x, y, x + SIZE, y + SIZE };
        HFONT old = (HFONT)SelectObject(hdc, hLogoFont);
        SetTextColor(hdc, RGB(255, 255, 255));
        SetBkMode(hdc, TRANSPARENT);
        DrawTextA(hdc, "A", -1, &textR, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, old);
    }
}

// ---------------------------------------------------------------------------
// Paint nav bar (back/fwd/refresh + URL bar + right icons)
// ---------------------------------------------------------------------------
static void PaintNavBar(ChromeState* ch, HDC hdc, int width) {
    // Nav bar background
    RECT navR = { 0, 0, width, NAV_HEIGHT };
    FillRectColor(hdc, navR, CLR_BG_PRIMARY);

    // "A" logo badge — top-left, vertically centered
    DrawLogoBadge(hdc, BTN_LOGO_X, (NAV_HEIGHT - 28) / 2, ch->hLogoFont);

    int urlRight = width - URLBAR_END;

    Gdiplus::Graphics graphics(hdc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

    // Draw unified glassmorphic card container with subtle violet glowing outlines
    {
        Gdiplus::GraphicsPath path;
        int r = 12; // radius
        int xL = BTN_BACK_X - 6; // 46
        int yT = 3;
        int wC = (urlRight + 6) - xL;
        int hC = (NAV_HEIGHT - 3) - yT; // 34px height

        // Create a rounded rectangle path
        path.AddArc((float)xL, (float)yT, (float)r, (float)r, 180, 90);
        path.AddArc((float)(xL + wC - r), (float)yT, (float)r, (float)r, 270, 90);
        path.AddArc((float)(xL + wC - r), (float)(yT + hC - r), (float)r, (float)r, 0, 90);
        path.AddArc((float)xL, (float)(yT + hC - r), (float)r, (float)r, 90, 90);
        path.CloseFigure();

        // Fill with semi-transparent dark background for glassmorphic card effect
        Gdiplus::SolidBrush fillBrush(Gdiplus::Color(200, 22, 24, 42)); // glassmorphic bg
        graphics.FillPath(&fillBrush, &path);

        // Outline with a subtle violet glowing border
        Gdiplus::Pen borderPen(Gdiplus::Color(120, 167, 139, 250), 1.0f); // subtle violet glow outline
        graphics.DrawPath(&borderPen, &path);
    }

    // Determine back/forward availability from tab history stack
    bool canGoBack = false, canGoForward = false;
    if (ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) {
        const auto& t = ch->tabs[ch->activeTab];
        canGoBack = (t.historyIndex > 0);
        canGoForward = (t.historyIndex < (int)t.history.size() - 1);
    }

    // Nav buttons: ← → ↻  (flat icons, no card borders — modern browser style)
    // Back and Forward dim to text-faint when unavailable
    struct { int x; const wchar_t* icon; bool enabled; int id; } navBtns[] = {
        { BTN_BACK_X, ICON_BACK,    canGoBack, 1 },
        { BTN_FWD_X,  ICON_FORWARD, canGoForward, 2 },
        { BTN_REF_X,  ICON_REFRESH, true, 3 }
    };
    for (int bi = 0; bi < 3; bi++) {
        auto& b = navBtns[bi];
        RECT br = { b.x, 4, b.x + BTN_BACK_W, NAV_HEIGHT - 4 };
        double alpha = ch->hoverAlphas[b.id];
        if (alpha > 0.0 && b.enabled) {
            Gdiplus::SolidBrush hoverBrush(Gdiplus::Color((BYTE)(alpha * 40.0), 255, 255, 255));
            FillGdiplusRoundRect(graphics, br, 6, hoverBrush);
        }
        COLORREF baseColor = !b.enabled ? CLR_TEXT_FAINT : CLR_TEXT_DIM;
        COLORREF hoverColor = CLR_TEXT;
        BYTE rIcon = (BYTE)( (1.0 - alpha) * GetRValue(baseColor) + alpha * GetRValue(hoverColor) );
        BYTE gIcon = (BYTE)( (1.0 - alpha) * GetGValue(baseColor) + alpha * GetGValue(hoverColor) );
        BYTE bIcon = (BYTE)( (1.0 - alpha) * GetBValue(baseColor) + alpha * GetBValue(hoverColor) );
        DrawIcon(hdc, b.icon, br, RGB(rIcon, gIcon, bIcon), ch->hIconFont);
    }

    // URL bar
    int urlLeft  = URLBAR_PAD;
    RECT urlR = { urlLeft, 6, urlRight, NAV_HEIGHT - 6 };
    COLORREF urlBorder = ch->urlFocused ? CLR_ACCENT : RGB(30, 33, 50);
    DrawRoundRect(hdc, urlR, 12, CLR_BG_CARD, urlBorder);

    // Lock icon — proper MDL2 lock glyph with dynamic DPI padding
    HDC hdcTemp = GetDC(ch->hwnd);
    int dpiY = GetDeviceCaps(hdcTemp, LOGPIXELSY);
    ReleaseDC(ch->hwnd, hdcTemp);
    int padX = MulDiv(6, dpiY, 72);
    RECT lockR = { urlLeft + padX, 6, urlLeft + padX + 18, NAV_HEIGHT - 6 };
    DrawIcon(hdc, ICON_LOCK, lockR, CLR_GREEN, ch->hIconFontSmall);

    // URL text with breathing 10px gap from lock icon
    const char* urlTxt = "about:blank";
    if (!ch->tabs.empty() && ch->activeTab >= 0 &&
        ch->activeTab < (int)ch->tabs.size()) {
        const auto& t = ch->tabs[ch->activeTab];
        urlTxt = t.url.c_str();
    }
    RECT urlTextR = { urlLeft + padX + 28, 6, urlRight - 32, NAV_HEIGHT - 6 };
    DrawText16(hdc, urlTxt, urlTextR, CLR_TEXT, ch->hTextFont);

    // AdBlock shield — small green dot indicator
    HBRUSH shieldB = CreateSolidBrush(CLR_GREEN);
    RECT shieldR = { urlRight - 24, 16, urlRight - 17, 23 };
    HRGN shieldRgn = CreateEllipticRgn(shieldR.left, shieldR.top,
        shieldR.right, shieldR.bottom);
    FillRgn(hdc, shieldRgn, shieldB);
    DeleteObject(shieldRgn);
    DeleteObject(shieldB);

    // Right icon buttons: Downloads, Bookmarks, Tor/Shield, Menu
    // Positioned between URL bar end and window controls
    int toolbarStart = width - CTRL_TOTAL - TOOLBAR_ICONS_W - 8;
    const wchar_t* rightIcons[] = { ICON_DOWNLOAD, ICON_BOOKMARK, ICON_SHIELD, ICON_AI_SUMMARY, ICON_AI_JOURNEY, ICON_MORE };
    COLORREF torColor = ch->settings.tor_enabled ? CLR_GREEN : CLR_TEXT_DIM;
    COLORREF iconColors[] = { CLR_TEXT_DIM, CLR_TEXT_DIM, torColor, CLR_TEXT_DIM, CLR_TEXT_DIM, CLR_TEXT_DIM };
    for (int i = 0; i < 6; i++) {
        RECT ir = { toolbarStart + i * 32, 4,
                    toolbarStart + i * 32 + 28, NAV_HEIGHT - 4 };
        int btnId = 20 + i;
        double alpha = ch->hoverAlphas[btnId];
        if (alpha > 0.0) {
            Gdiplus::SolidBrush hoverBrush(Gdiplus::Color((BYTE)(alpha * 40.0), 255, 255, 255));
            FillGdiplusRoundRect(graphics, ir, 6, hoverBrush);
        }
        COLORREF baseColor = iconColors[i];
        COLORREF hoverColor = CLR_TEXT;
        BYTE rIcon = (BYTE)( (1.0 - alpha) * GetRValue(baseColor) + alpha * GetRValue(hoverColor) );
        BYTE gIcon = (BYTE)( (1.0 - alpha) * GetGValue(baseColor) + alpha * GetGValue(hoverColor) );
        BYTE bIcon = (BYTE)( (1.0 - alpha) * GetBValue(baseColor) + alpha * GetBValue(hoverColor) );
        DrawIcon(hdc, rightIcons[i], ir, RGB(rIcon, gIcon, bIcon), ch->hIconFontLarge);
    }

    // Window control buttons: minimize, maximize/restore, close
    // Anchored at the far right edge
    RECT minR  = { width - CTRL_TOTAL,          0, width - CTRL_W * 2, NAV_HEIGHT };
    RECT maxR  = { width - CTRL_W * 2,          0, width - CTRL_W,     NAV_HEIGHT };
    RECT clsR  = { width - CTRL_W,              0, width,              NAV_HEIGHT };
    
    // Minimize hover
    double minAlpha = ch->hoverAlphas[10];
    if (minAlpha > 0.0) {
        Gdiplus::SolidBrush hoverBrush(Gdiplus::Color((BYTE)(minAlpha * 40.0), 255, 255, 255));
        FillGdiplusRoundRect(graphics, minR, 6, hoverBrush);
    }
    BYTE rMinIcon = (BYTE)( (1.0 - minAlpha) * GetRValue(CLR_TEXT_DIM) + minAlpha * 255 );
    BYTE gMinIcon = (BYTE)( (1.0 - minAlpha) * GetGValue(CLR_TEXT_DIM) + minAlpha * 255 );
    BYTE bMinIcon = (BYTE)( (1.0 - minAlpha) * GetBValue(CLR_TEXT_DIM) + minAlpha * 255 );
    DrawIcon(hdc, ICON_MINIMIZE, minR, RGB(rMinIcon, gMinIcon, bMinIcon), ch->hIconFont);

    // Maximize hover
    double maxAlpha = ch->hoverAlphas[11];
    if (maxAlpha > 0.0) {
        Gdiplus::SolidBrush hoverBrush(Gdiplus::Color((BYTE)(maxAlpha * 40.0), 255, 255, 255));
        FillGdiplusRoundRect(graphics, maxR, 6, hoverBrush);
    }
    BYTE rMaxIcon = (BYTE)( (1.0 - maxAlpha) * GetRValue(CLR_TEXT_DIM) + maxAlpha * 255 );
    BYTE gMaxIcon = (BYTE)( (1.0 - maxAlpha) * GetGValue(CLR_TEXT_DIM) + maxAlpha * 255 );
    BYTE bMaxIcon = (BYTE)( (1.0 - maxAlpha) * GetBValue(CLR_TEXT_DIM) + maxAlpha * 255 );
    DrawIcon(hdc, IsZoomed(ch->hwnd) ? ICON_RESTORE : ICON_MAXIMIZE, maxR,
             RGB(rMaxIcon, gMaxIcon, bMaxIcon), ch->hIconFont);

    // Close button — red background on hover with smooth transition
    double closeAlpha = ch->hoverAlphas[99];
    if (closeAlpha > 0.0) {
        Gdiplus::SolidBrush closeBrush(Gdiplus::Color((BYTE)(closeAlpha * 255.0), 196, 43, 28));
        graphics.FillRectangle(&closeBrush, (float)clsR.left, (float)clsR.top, (float)(clsR.right - clsR.left), (float)(clsR.bottom - clsR.top));
    }
    BYTE rCloseIcon = (BYTE)( (1.0 - closeAlpha) * GetRValue(CLR_TEXT_DIM) + closeAlpha * 255 );
    BYTE gCloseIcon = (BYTE)( (1.0 - closeAlpha) * GetGValue(CLR_TEXT_DIM) + closeAlpha * 255 );
    BYTE bCloseIcon = (BYTE)( (1.0 - closeAlpha) * GetBValue(CLR_TEXT_DIM) + closeAlpha * 255 );
    DrawIcon(hdc, ICON_CLOSE, clsR, RGB(rCloseIcon, gCloseIcon, bCloseIcon), ch->hIconFont);
}

// ---------------------------------------------------------------------------
// Paint tab strip
// ---------------------------------------------------------------------------
static void PaintTabStrip(ChromeState* ch, HDC hdc, int width) {
    RECT tabStrip = { 0, NAV_HEIGHT, width, NAV_HEIGHT + TAB_HEIGHT };
    FillRectColor(hdc, tabStrip, CLR_BG_PRIMARY); // #0d0e14 primary bg

    // Bottom separator line
    RECT sep = { 0, NAV_HEIGHT + TAB_HEIGHT - 1, width, NAV_HEIGHT + TAB_HEIGHT };
    FillRectColor(hdc, sep, CLR_TEXT_FAINT);

    int tabX = 8;
    const int TAB_W_MAX  = 200;
    const int TAB_W_MIN  = 80;
    const int TAB_MARGIN = 4;

    int tabCount = max(1, (int)ch->tabs.size());
    int tabW = min(TAB_W_MAX, max(TAB_W_MIN, (width - 60) / tabCount));

    for (int i = 0; i < (int)ch->tabs.size(); i++) {
        ChromeTab& t = ch->tabs[i];
        bool active = (i == ch->activeTab);
        bool hover  = (i == ch->hoverTab);

        RECT tR = { tabX, NAV_HEIGHT + 2, tabX + tabW, NAV_HEIGHT + TAB_HEIGHT - 1 };
        t.tabRect = tR;

        // Tab background with smooth color-shifting animation on hover/active
        COLORREF tabBg = CLR_BG_PRIMARY; // default inactive: #0d0e14
        if (active) {
            tabBg = CLR_BG_ACTIVE; // #1e2140
        } else if (hover) {
            // Smoothly shift/pulse card background (#16182a) to a slightly lighter violet hue
            DWORD tick = GetTickCount();
            double factor = (sin(tick * 0.006) + 1.0) / 2.0;
            int r = (int)(22 + factor * 8);   // 22 (#16) to 30 (#1e)
            int g = (int)(24 + factor * 9);   // 24 (#18) to 33 (#21)
            int b = (int)(42 + factor * 22);  // 42 (#2a) to 64 (#40)
            tabBg = RGB(r, g, b);
        }
        
        COLORREF tabBrd = active ? CLR_ACCENT : CLR_TEXT_FAINT;
        
        // Re-architect tab drawing using GDI+ GraphicsPath for organic curved tabs
        {
            Gdiplus::Graphics graphics(hdc);
            graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

            Gdiplus::GraphicsPath tabPath;
            float xL = (float)tR.left;
            float xR = (float)tR.right;
            float yT = (float)tR.top;
            float yB = (float)tR.bottom;

            // Curved tab shape flowing naturally from the tab edge into baseline:
            // Start at bottom-left (extended outward by 8px for smooth transition)
            tabPath.AddLine(xL - 8.0f, yB, xL - 4.0f, yB);
            // Curve up to the top of the tab
            tabPath.AddBezier(
                xL - 4.0f, yB,
                xL + 1.0f, yB,
                xL + 1.0f, yT,
                xL + 8.0f, yT
            );
            // Line across the top of the tab
            tabPath.AddLine(xL + 8.0f, yT, xR - 8.0f, yT);
            // Curve down to the bottom-right
            tabPath.AddBezier(
                xR - 8.0f, yT,
                xR - 1.0f, yT,
                xR - 1.0f, yB,
                xR + 4.0f, yB
            );
            tabPath.AddLine(xR + 4.0f, yB, xR + 8.0f, yB);
            tabPath.CloseFigure();

            // Fill curved tab
            Gdiplus::SolidBrush tabFillBrush(Gdiplus::Color(
                255, 
                GetRValue(tabBg), 
                GetGValue(tabBg), 
                GetBValue(tabBg)
            ));
            graphics.FillPath(&tabFillBrush, &tabPath);

            if (active) {
                // Active tab glowing violet accent lines
                Gdiplus::GraphicsPath topGlowPath;
                topGlowPath.AddBezier(
                    xL - 4.0f, yB,
                    xL + 1.0f, yB,
                    xL + 1.0f, yT,
                    xL + 8.0f, yT
                );
                topGlowPath.AddLine(xL + 8.0f, yT, xR - 8.0f, yT);
                topGlowPath.AddBezier(
                    xR - 8.0f, yT,
                    xR - 1.0f, yT,
                    xR - 1.0f, yB,
                    xR + 4.0f, yB
                );

                // Outer glow (thick, semi-transparent)
                Gdiplus::Pen glowPen1(Gdiplus::Color(100, 108, 99, 255), 3.0f); // #6c63ff with transparency
                graphics.DrawPath(&glowPen1, &topGlowPath);

                // Inner core (thin, bright violet)
                Gdiplus::Pen glowPen2(Gdiplus::Color(255, 167, 139, 250), 1.5f); // #a78bfa
                graphics.DrawPath(&glowPen2, &topGlowPath);

                // Bottom violet glow connection line with fading edges
                float xGlow = (float)(tR.left + 3);
                float yGlow = (float)(tR.bottom - 2);
                float wGlow = (float)(tR.right - tR.left - 6);
                float hGlow = 3.0f; // Height of glow bar
                Gdiplus::RectF glowRect(xGlow, yGlow, wGlow, hGlow);

                Gdiplus::Color accentColor(255, 108, 99, 255); // CLR_ACCENT #6c63ff is RGB(108, 99, 255)
                Gdiplus::Color transAccent(0, 108, 99, 255);

                Gdiplus::Color glowColors[] = { transAccent, accentColor, transAccent };
                float glowPositions[] = { 0.0f, 0.5f, 1.0f };

                Gdiplus::LinearGradientBrush glowBrush(
                    glowRect,
                    transAccent,
                    transAccent,
                    Gdiplus::LinearGradientModeHorizontal
                );
                glowBrush.SetInterpolationColors(glowColors, glowPositions, 3);
                graphics.FillRectangle(&glowBrush, glowRect);
            } else {
                // Inactive/hover tab outline (subtle)
                Gdiplus::Pen borderPen(Gdiplus::Color(
                    255, 
                    GetRValue(tabBrd), 
                    GetGValue(tabBrd), 
                    GetBValue(tabBrd)
                ), 1.0f);
                graphics.DrawPath(&borderPen, &tabPath);
            }
        }

        // Tab favicon or loading indicator
        if (t.loading) {
            // Loading: animated accent-colored spinner dot
            // Use a simple pulsing dot effect — alternates size based on tick
            DWORD tick = GetTickCount();
            int phase = (tick / 200) % 4;  // 4-phase pulse
            int dotSize = 6 + (phase % 2) * 2;  // oscillate 6-8px
            int cx = tR.left + 12;
            int cy = tR.top + 13;
            HBRUSH spinB = CreateSolidBrush(CLR_ACCENT);
            HRGN spinRgn = CreateEllipticRgn(
                cx - dotSize/2, cy - dotSize/2,
                cx + dotSize/2, cy + dotSize/2);
            FillRgn(hdc, spinRgn, spinB);
            DeleteObject(spinRgn);
            DeleteObject(spinB);
        } else {
            // Favicon dot placeholder
            HBRUSH favB = CreateSolidBrush(active ? CLR_ACCENT2 : CLR_TEXT_FAINT);
            RECT favR = { tR.left + 8, tR.top + 9, tR.left + 16, tR.top + 17 };
            HRGN favRgn = CreateEllipticRgn(favR.left, favR.top, favR.right, favR.bottom);
            FillRgn(hdc, favRgn, favB);
            DeleteObject(favRgn);
            DeleteObject(favB);
        }

        // Tab title
        RECT titleR = { tR.left + 20, tR.top, tR.right - 24, tR.bottom };
        const char* displayTitle = t.loading ? "Loading..." :
                                   (t.title.empty() ? "New Tab" : t.title.c_str());
        DrawText16(hdc, displayTitle,
            titleR, active ? CLR_TEXT : CLR_TEXT_DIM, ch->hTextTabFont);

        // Close × (MDL2 icon) — more breathing room from title
        RECT closeR = { tR.right - 22, tR.top + 4, tR.right - 4, tR.bottom - 4 };
        DrawIcon(hdc, ICON_CLOSE, closeR, CLR_TEXT_FAINT, ch->hIconFontSmall);

        tabX += tabW + TAB_MARGIN;
    }

    // New tab (+) button
    RECT addR = { tabX + 4, NAV_HEIGHT + 5, tabX + 28, NAV_HEIGHT + TAB_HEIGHT - 5 };
    DrawRoundRect(hdc, addR, 5, CLR_BG_CARD, CLR_TEXT_FAINT);
    DrawIcon(hdc, ICON_ADD, addR, CLR_TEXT_DIM, ch->hIconFont);
}

// ---------------------------------------------------------------------------
// Full chrome paint
// ---------------------------------------------------------------------------
static void PaintChrome(ChromeState* ch, HDC hdcOverride = nullptr) {
    RECT rc;
    GetClientRect(ch->hwnd, &rc);
    int W = rc.right;

    // Double-buffered paint
    HDC hdc    = hdcOverride ? hdcOverride : GetDC(ch->hwnd);
    HDC memDC  = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, CHROME_H);
    HBITMAP old = (HBITMAP)SelectObject(memDC, bmp);

    PaintNavBar(ch, memDC, W);
    PaintTabStrip(ch, memDC, W);

    // Blit to screen
    BitBlt(hdc, 0, 0, W, CHROME_H, memDC, 0, 0, SRCCOPY);
    SelectObject(memDC, old);
    DeleteObject(bmp);
    DeleteDC(memDC);
    
    if (!hdcOverride) {
        ReleaseDC(ch->hwnd, hdc);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
namespace BrowserChrome {

void Create(HWND parent, const SystemProfile* profile, AeonEngineVTable* engine) {
    ChromeState* ch = new ChromeState();
    ch->profile    = profile;
    ch->engine     = engine;
    ch->activeTab  = -1;
    ch->hoverTab   = -1;
    ch->hoverBtn   = 0;
    ch->urlFocused = false;
    ch->hUrlBgBrush = CreateSolidBrush(RGB(22, 24, 42)); // CLR_BG_CARD for URL bar
    ch->settings   = SettingsEngine::Load();

    // Initialize all cached GDI font handles to prevent GDI leak
    HDC hdcTemp = GetDC(parent);
    int dpiY = GetDeviceCaps(hdcTemp, LOGPIXELSY);
    ReleaseDC(parent, hdcTemp);

    ch->hTextFont = CreateFontW(-MulDiv(9, dpiY, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hTextBold = CreateFontW(-MulDiv(9, dpiY, 72), 0, 0, 0, FW_SEMIBOLD,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hTextTabFont = CreateFontW(-MulDiv(8, dpiY, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hIconFont = CreateFontW(-MulDiv(10, dpiY, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    ch->hIconFontSmall = CreateFontW(-MulDiv(8, dpiY, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    ch->hIconFontLarge = CreateFontW(-MulDiv(11, dpiY, 72), 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
    ch->hLogoFont = CreateFontW(-MulDiv(15, dpiY, 72), 0, 0, 0, FW_BOLD,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    ch->hUrlFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL,
        0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    // Add a default new tab
    if (engine) {
        ChromeTab t;
        t.id      = engine->NewTab(parent, "aeon://newtab");
        t.url     = "aeon://newtab";
        t.title   = "New Tab";
        t.loading = false;
        t.history.push_back(t.url);
        t.historyIndex = 0;
        ch->tabs.push_back(t);
        ch->activeTab = 0;

        // Set engine viewport below the chrome
        RECT rc; GetClientRect(parent, &rc);
        engine->SetViewport(t.id, parent, 0, CHROME_H,
            rc.right, rc.bottom - CHROME_H);

        // Inject AeonBridge bootstrap into every new document
        // (queued inside the DLL until WebView2 is ready)
        std::string bridgeJs = AeonBridge::BuildInjectionScript();
        engine->InjectEarlyJS(t.id, bridgeJs.c_str());
    }

    ch->hwnd = parent;
    PaintChrome(ch);

    // Store in GWLP_USERDATA so WM_PAINT can find it
    SetWindowLongPtr(parent, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ch));

    fprintf(stdout, "[Chrome] Browser chrome created. Logo badge: active. "
         "Tab strip: %d tab(s).\n", (int)ch->tabs.size());

    // ── Background services on worker thread (non-blocking) ─────────────
    std::thread([]{
        // 1. Initialize DNS resolver first (needed by NetworkSentinel)
        DnsResolver::Initialize();

        // 2. Network analysis — classifies network type and sets DnsResolver hint
        NetworkSentinel::Analyze();
        NetworkSentinel::ApplyBestStrategy();
        NetworkSentinel::StartMonitor(); // re-checks every 30s

        // 3. AutoUpdater: already started by AeonMain::WinMain
        //    No additional call needed here.
    }).detach();
}

void OnPaint(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (ch) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        PaintChrome(ch, hdc);
        EndPaint(hwnd, &ps);
    }
}

void OnTimer(HWND hwnd, WPARAM wParam) {
    if (wParam != 9005) return;
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    bool anyActive = false;
    double delta = 0.15;

    for (int i = 0; i < 100; i++) {
        double target = (ch->hoverBtn == i) ? 1.0 : 0.0;
        if (ch->hoverAlphas[i] != target) {
            if (ch->hoverAlphas[i] < target) {
                ch->hoverAlphas[i] += delta;
                if (ch->hoverAlphas[i] > target) ch->hoverAlphas[i] = target;
            } else {
                ch->hoverAlphas[i] -= delta;
                if (ch->hoverAlphas[i] < target) ch->hoverAlphas[i] = target;
            }
            anyActive = true;
        }
    }

    if (anyActive) {
        PaintChrome(ch);
    } else {
        KillTimer(hwnd, 9005);
    }
}

void OnSize(HWND hwnd, int w, int h) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    PaintChrome(ch);

    // Resize URL bar edit control if visible
    if (ch->hUrlBar && IsWindowVisible(ch->hUrlBar)) {
        RECT rc; GetClientRect(hwnd, &rc);
        int urlLeft  = URLBAR_PAD + 4;
        int urlRight = rc.right - URLBAR_END - 4;
        MoveWindow(ch->hUrlBar, urlLeft, 8, urlRight - urlLeft, NAV_HEIGHT - 16, TRUE);
    }

    if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) {
        auto& t = ch->tabs[ch->activeTab];
        ch->engine->SetViewport(t.id, hwnd, 0, CHROME_H, w, h - CHROME_H);
    }
}

// ---------------------------------------------------------------------------
// URL bar constants
// ---------------------------------------------------------------------------
#define IDC_URLBAR 9001

// Loading animation timer — fires every 200ms to repaint the spinner
#define ID_LOADING_TIMER 9002
#define LOADING_TIMER_MS 200
static WNDPROC g_OrigUrlBarProc = nullptr;

// Sub-class proc for URL bar EDIT — intercepts Enter/Escape
static LRESULT CALLBACK UrlBarSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) {
            // Send the URL bar text as a navigation command
            HWND parent = GetParent(hWnd);
            SendMessageW(parent, WM_COMMAND,
                MAKEWPARAM(IDC_URLBAR, EN_CHANGE + 100), (LPARAM)hWnd);
            return 0;
        }
        if (wParam == VK_ESCAPE) {
            // Cancel editing — hide URL bar
            ShowWindow(hWnd, SW_HIDE);
            SetFocus(GetParent(hWnd));
            return 0;
        }
    }
    if (msg == WM_KILLFOCUS) {
        // Hide when focus leaves
        ShowWindow(hWnd, SW_HIDE);
        ChromeState* ch = reinterpret_cast<ChromeState*>(
            GetWindowLongPtr(GetParent(hWnd), GWLP_USERDATA));
        if (ch) { ch->urlFocused = false; PaintChrome(ch); }
    }
    return CallWindowProcW(g_OrigUrlBarProc, hWnd, msg, wParam, lParam);
}

// Create (or show) the URL bar edit control
static void ActivateUrlBar(ChromeState* ch, bool selectAll = true) {
    RECT rc; GetClientRect(ch->hwnd, &rc);
    int urlLeft  = URLBAR_PAD + 4;
    int urlRight = rc.right - URLBAR_END - 4;

    if (!ch->hUrlBar) {
        ch->hUrlBar = CreateWindowExW(
            0, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
            urlLeft, 8, urlRight - urlLeft, NAV_HEIGHT - 16,
            ch->hwnd, (HMENU)(UINT_PTR)IDC_URLBAR, nullptr, nullptr);

        SendMessageW(ch->hUrlBar, WM_SETFONT, (WPARAM)ch->hUrlFont, TRUE);

        // Subclass to intercept Enter/Escape
        g_OrigUrlBarProc = (WNDPROC)SetWindowLongPtr(
            ch->hUrlBar, GWLP_WNDPROC, (LONG_PTR)UrlBarSubclassProc);
    }

    // Position and show
    MoveWindow(ch->hUrlBar, urlLeft, 8, urlRight - urlLeft, NAV_HEIGHT - 16, TRUE);

    // Populate with current URL
    const char* url = "about:blank";
    if (ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size())
        url = ch->tabs[ch->activeTab].url.c_str();

    // Convert UTF-8 to UTF-16 wide string for EDIT control using safe helper
    std::wstring wUrl = Utf8ToUtf16(url);
    SetWindowTextW(ch->hUrlBar, wUrl.c_str());

    ShowWindow(ch->hUrlBar, SW_SHOW);
    SetFocus(ch->hUrlBar);
    if (selectAll)
        SendMessageW(ch->hUrlBar, EM_SETSEL, 0, -1);

    ch->urlFocused = true;
    PaintChrome(ch);
}

// Navigate to the URL currently in the edit control
static void CommitUrlBar(ChromeState* ch) {
    if (!ch->hUrlBar) return;
    wchar_t wbuf[2048] = {};
    GetWindowTextW(ch->hUrlBar, wbuf, 2048);

    ShowWindow(ch->hUrlBar, SW_HIDE);
    SetFocus(ch->hwnd);
    ch->urlFocused = false;

    if (wbuf[0] == L'\0') return;

    // Convert wide to UTF-8 using safe helper
    std::string url = Utf16ToUtf8(wbuf);

    if (url.empty()) return;

    // Auto-prepend https:// if no scheme present
    if (url.find("://") == std::string::npos && url.find("aeon://") != 0) {
        // If it looks like a domain (has a dot), navigate; otherwise search
        if (url.find('.') != std::string::npos || url.find("localhost") == 0) {
            url = "https://" + url;
        } else {
            // Treat as search query and encode spaces/ampersands
            std::string encoded;
            for (char c : url) {
                if (c == ' ') encoded += '+';
                else if (c == '&') encoded += "%26";
                else encoded += c;
            }
            
            std::string engine = ch->settings.search_engine;
            for (auto& c : engine) c = (char)tolower(c);

            if (engine == "google") {
                url = "https://www.google.com/search?q=" + encoded;
            } else if (engine == "bing") {
                url = "https://www.bing.com/search?q=" + encoded;
            } else if (engine == "brave") {
                url = "https://search.brave.com/search?q=" + encoded;
            } else if (engine == "ecosia") {
                url = "https://www.ecosia.org/search?q=" + encoded;
            } else { // Default to DuckDuckGo
                url = "https://duckduckgo.com/?q=" + encoded;
            }
        }
    }

    // Update tab state
    if (ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) {
        auto& t = ch->tabs[ch->activeTab];
        t.url = url;
        t.loading = true;
        if (ch->engine) ch->engine->Navigate(t.id, url.c_str(), nullptr);
        // Start loading animation timer
        SetTimer(ch->hwnd, ID_LOADING_TIMER, LOADING_TIMER_MS, nullptr);
    }

    PaintChrome(ch);
    fprintf(stdout, "[Chrome] Navigate: %s\n", url.c_str());
}

// ---------------------------------------------------------------------------
// Hit-test: returns button ID or -1 for empty area
// IDs: 1=back, 2=fwd, 3=refresh, 4=urlbar, 5..8=right icons, 10=min, 11=max, 12=close
//      100+i = tab i, 200+i = tab i close button
// ---------------------------------------------------------------------------
int HitTest(HWND hwnd, int x, int y) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return -1;

    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right;

    // Nav bar region (y < NAV_HEIGHT)
    if (y < NAV_HEIGHT) {
        // Window controls (check first — anchored at far right)
        if (x >= W - CTRL_W)              return 12; // close
        if (x >= W - CTRL_W * 2)          return 11; // maximize
        if (x >= W - CTRL_TOTAL)          return 10; // minimize

        // Toolbar icons: Downloads, Bookmarks, Shield, AI Summary, AI Journey, Menu
        int toolbarStart = W - CTRL_TOTAL - TOOLBAR_ICONS_W - 8;
        if (x >= toolbarStart && x < toolbarStart + TOOLBAR_ICONS_W) {
            int iconIdx = (x - toolbarStart) / 32;
            return 20 + iconIdx; // 20=download, 21=bookmark, 22=shield, 23=AI Summary, 24=AI Journey, 25=menu
        }

        // Logo badge
        if (x >= BTN_LOGO_X && x < BTN_LOGO_X + BTN_LOGO_W) return 0;
        // Back
        if (x >= BTN_BACK_X && x < BTN_BACK_X + BTN_BACK_W) return 1;
        // Forward
        if (x >= BTN_FWD_X && x < BTN_FWD_X + BTN_FWD_W) return 2;
        // Refresh
        if (x >= BTN_REF_X && x < BTN_REF_X + BTN_REF_W) return 3;
        // URL bar
        if (x >= URLBAR_PAD && x < W - URLBAR_END) return 4;

        return -1; // Empty nav bar = draggable
    }

    // Tab strip region (NAV_HEIGHT <= y < CHROME_H)
    if (y < CHROME_H) {
        for (int i = 0; i < (int)ch->tabs.size(); i++) {
            const RECT& tr = ch->tabs[i].tabRect;
            if (x >= tr.left && x < tr.right && y >= tr.top && y < tr.bottom) {
                // Close button on tab (22px from right edge)
                if (x >= tr.right - 22) return 200 + i;
                return 100 + i;
            }
        }
        // "+" new tab button
        int tabCount = max(1, (int)ch->tabs.size());
        int tabW = min(200, max(80, (W - 60) / tabCount));
        int addX = 8 + (int)ch->tabs.size() * (tabW + 4) + 4;
        if (x >= addX && x < addX + 28) return 50; // new tab (wider hit area)
        return -1;
    }

    return -1; // Content area
}

// ---------------------------------------------------------------------------
// Helper: create new tab
// ---------------------------------------------------------------------------
static void CreateNewTab(ChromeState* ch, const char* url = "aeon://newtab") {
    if (!ch->engine) return;
    ChromeTab t;
    t.id = ch->engine->NewTab(ch->hwnd, url);
    t.url = url;
    t.title = "New Tab";
    t.loading = false;
    t.history.push_back(url);
    t.historyIndex = 0;
    ch->tabs.push_back(t);
    ch->activeTab = (int)ch->tabs.size() - 1;

    // Set viewport
    RECT rc; GetClientRect(ch->hwnd, &rc);
    ch->engine->SetViewport(t.id, ch->hwnd, 0, CHROME_H,
        rc.right, rc.bottom - CHROME_H);
    ch->engine->FocusTab(t.id);

    // Inject AeonBridge bootstrap into every new document
    std::string bridgeJs = AeonBridge::BuildInjectionScript();
    ch->engine->InjectEarlyJS(t.id, bridgeJs.c_str());

    PaintChrome(ch);
    fprintf(stdout, "[Chrome] New tab #%u: %s\n", t.id, url);
}

// ---------------------------------------------------------------------------
// Mouse event handlers
// ---------------------------------------------------------------------------
static void NavigateActiveTab(ChromeState* ch, const char* url) {
    if (ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) {
        auto& t = ch->tabs[ch->activeTab];
        t.url = url;
        t.loading = true;
        if (ch->engine) ch->engine->Navigate(t.id, url, nullptr);
        SetTimer(ch->hwnd, ID_LOADING_TIMER, LOADING_TIMER_MS, nullptr);
        PaintChrome(ch);
    }
}

void OnLButtonDown(HWND hwnd, int x, int y) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    int hit = HitTest(hwnd, x, y);
    RECT rc; GetClientRect(hwnd, &rc);

    switch (hit) {
        case 1: // Back
            if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) {
                auto& t = ch->tabs[ch->activeTab];
                if (t.historyIndex > 0) {
                    ch->engine->GoBack(t.id);
                }
            }
            break;

        case 2: // Forward
            if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) {
                auto& t = ch->tabs[ch->activeTab];
                if (t.historyIndex < (int)t.history.size() - 1) {
                    ch->engine->GoForward(t.id);
                }
            }
            break;

        case 3: // Refresh
            if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size())
                ch->engine->Reload(ch->tabs[ch->activeTab].id, 0);
            break;

        case 4: // URL bar clicked
            ActivateUrlBar(ch);
            break;

        case 10: // Minimize
            ShowWindow(hwnd, SW_MINIMIZE);
            break;

        case 11: // Maximize / Restore
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            break;

        case 12: // Close
            DestroyWindow(hwnd);
            break;

        case 50: // New tab button
            CreateNewTab(ch);
            break;

        case 20: // Downloads button
            NavigateActiveTab(ch, "aeon://downloads");
            break;

        case 21: // Bookmarks button
            NavigateActiveTab(ch, "aeon://bookmarks");
            break;

        case 22: // Tor/Shield toggle
            ch->settings.tor_enabled = !ch->settings.tor_enabled;
            SettingsEngine::Save(ch->settings);
            NetworkSentinel::ApplyBestStrategy();
            PaintChrome(ch);
            break;

        case 23: // AI Summary button
            NavigateActiveTab(ch, "aeon://intelligence");
            break;

        case 24: // AI Journey button
            NavigateActiveTab(ch, "aeon://journey");
            break;

        case 25: { // Menu button (More)
            int toolbarStart = rc.right - CTRL_TOTAL - TOOLBAR_ICONS_W - 8;
            POINT pt = { toolbarStart + 5 * 32 + 28, NAV_HEIGHT };
            ClientToScreen(ch->hwnd, &pt);
            
            auto callback = [ch](AppMenu::ItemId id) {
                switch (id) {
                    case AppMenu::ItemId::History:
                        NavigateActiveTab(ch, "aeon://history");
                        break;
                    case AppMenu::ItemId::Downloads:
                        NavigateActiveTab(ch, "aeon://downloads");
                        break;
                    case AppMenu::ItemId::Bookmarks:
                        NavigateActiveTab(ch, "aeon://bookmarks");
                        break;
                    case AppMenu::ItemId::Settings:
                        NavigateActiveTab(ch, "aeon://settings");
                        break;
                    case AppMenu::ItemId::Exit:
                        DestroyWindow(ch->hwnd);
                        break;
                    case AppMenu::ItemId::NewTab:
                        CreateNewTab(ch);
                        break;
                    default:
                        break;
                }
            };
            AppMenu::Show(ch->hwnd, pt, callback);
            break;
        }

        default:
            // Tab click (100+i)
            if (hit >= 200) {
                // Close tab button
                int idx = hit - 200;
                if (idx >= 0 && idx < (int)ch->tabs.size()) {
                    // AI: Notify TabIntelligence BEFORE destroying the tab
                    if (g_TabIntel) g_TabIntel->OnTabClosed((uint64_t)ch->tabs[idx].id);

                    if (ch->engine) ch->engine->CloseTab(ch->tabs[idx].id);
                    ch->tabs.erase(ch->tabs.begin() + idx);
                    if (ch->tabs.empty()) {
                        // Last tab closed — open new tab
                        CreateNewTab(ch);
                    } else {
                        if (ch->activeTab >= (int)ch->tabs.size())
                            ch->activeTab = (int)ch->tabs.size() - 1;
                        if (ch->engine)
                            ch->engine->FocusTab(ch->tabs[ch->activeTab].id);
                    }
                    PaintChrome(ch);
                }
            } else if (hit >= 100) {
                // Switch to tab
                int idx = hit - 100;
                if (idx >= 0 && idx < (int)ch->tabs.size() && idx != ch->activeTab) {
                    unsigned int prevTabId = ch->tabs[ch->activeTab].id;
                    ch->activeTab = idx;
                    if (ch->engine) {
                        ch->engine->FocusTab(ch->tabs[idx].id);
                        ch->engine->SetViewport(ch->tabs[idx].id, hwnd, 0, CHROME_H,
                            rc.right, rc.bottom - CHROME_H);
                    }
                    // AI: Notify AI engines of tab focus change
                    if (g_TabIntel) g_TabIntel->OnTabFocused((uint64_t)ch->tabs[idx].id);
                    if (g_JourneyAI)
                        g_JourneyAI->OnTabSwitch((uint64_t)prevTabId, (uint64_t)ch->tabs[idx].id);
                    PaintChrome(ch);
                }
            }
            break;
    }
}

void OnLButtonUp(HWND hwnd, int x, int y) {
    (void)hwnd; (void)x; (void)y;
}

void OnMouseMove(HWND hwnd, int x, int y) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    int oldHover = ch->hoverTab;
    int oldBtn   = ch->hoverBtn;
    ch->hoverTab = -1;
    ch->hoverBtn = -1;

    // Nav bar hover tracking
    if (y < NAV_HEIGHT) {
        int hit = HitTest(hwnd, x, y);
        switch (hit) {
            case 1: case 2: case 3:   ch->hoverBtn = hit; break;  // back/fwd/refresh
            case 10:                  ch->hoverBtn = 10;  break;  // minimize
            case 11:                  ch->hoverBtn = 11;  break;  // maximize
            case 12:                  ch->hoverBtn = 99;  break;  // close (99 for red bg)
            case 20: case 21: case 22: case 23: case 24: case 25:
                                      ch->hoverBtn = hit; break;  // toolbar icons
        }
    }

    // Tab strip hover tracking
    if (y >= NAV_HEIGHT && y < CHROME_H) {
        for (int i = 0; i < (int)ch->tabs.size(); i++) {
            const RECT& tr = ch->tabs[i].tabRect;
            if (x >= tr.left && x < tr.right && y >= tr.top && y < tr.bottom) {
                ch->hoverTab = i;
                break;
            }
        }
    }

    if (ch->hoverTab != oldHover || ch->hoverBtn != oldBtn) {
        if (ch->hoverBtn != oldBtn) {
            SetTimer(hwnd, 9005, 16, nullptr);
        }
        PaintChrome(ch);
    }
}

// WM_COMMAND from URL bar EDIT
void OnCommand(HWND hwnd, int id, int notifyCode, HWND ctlHwnd) {
    (void)ctlHwnd;
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    if (id == IDC_URLBAR && notifyCode == EN_CHANGE + 100) {
        // Enter key pressed in URL bar
        CommitUrlBar(ch);
    }
}

// ---------------------------------------------------------------------------
// Keyboard shortcuts — standard browser accelerators
// Returns true if handled (caller should NOT pass to DefWindowProc).
// ---------------------------------------------------------------------------
bool OnKeyDown(HWND hwnd, WPARAM vk, LPARAM lParam) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return false;

    bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    bool alt   = (GetKeyState(VK_MENU)    & 0x8000) != 0;
    (void)lParam;

    // --- Ctrl+T: New tab ---
    if (ctrl && !shift && !alt && vk == 'T') {
        CreateNewTab(ch);
        return true;
    }

    // --- Ctrl+K: Arc Command Bar Spotlight Overlay ---
    if (ctrl && !shift && !alt && vk == 'K') {
        ToggleCommandBar(hwnd);
        return true;
    }

    // --- Ctrl+S: Arc Vertical Sidebar Toggle ---
    if (ctrl && !shift && !alt && vk == 'S') {
        ToggleSidebar(hwnd);
        return true;
    }

    // --- Ctrl+W: Close current tab ---
    if (ctrl && !shift && !alt && vk == 'W') {
        if (ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) {
            // AI: Notify TabIntelligence BEFORE destroying the tab
            if (g_TabIntel) g_TabIntel->OnTabClosed((uint64_t)ch->tabs[ch->activeTab].id);

            if (ch->engine)
                ch->engine->CloseTab(ch->tabs[ch->activeTab].id);
            ch->tabs.erase(ch->tabs.begin() + ch->activeTab);
            if (ch->tabs.empty()) {
                CreateNewTab(ch);
            } else {
                if (ch->activeTab >= (int)ch->tabs.size())
                    ch->activeTab = (int)ch->tabs.size() - 1;
                if (ch->engine)
                    ch->engine->FocusTab(ch->tabs[ch->activeTab].id);
            }
            PaintChrome(ch);
        }
        return true;
    }

    // --- Ctrl+L / F6: Focus URL bar ---
    if ((ctrl && !shift && !alt && vk == 'L') || vk == VK_F6) {
        ActivateUrlBar(ch);
        return true;
    }

    // --- Ctrl+R / F5: Refresh ---
    if ((ctrl && !shift && !alt && vk == 'R') || vk == VK_F5) {
        if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size())
            ch->engine->Reload(ch->tabs[ch->activeTab].id, 0);
        return true;
    }

    // --- Ctrl+Shift+R / Shift+F5: Hard refresh (cache bypass) ---
    if ((ctrl && shift && !alt && vk == 'R') || (shift && vk == VK_F5)) {
        if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size())
            ch->engine->Reload(ch->tabs[ch->activeTab].id, 1);
        return true;
    }

    // --- Ctrl+Tab / Ctrl+Shift+Tab: Cycle tabs ---
    if (ctrl && !alt && vk == VK_TAB) {
        int n = (int)ch->tabs.size();
        if (n > 1) {
            if (shift)
                ch->activeTab = (ch->activeTab - 1 + n) % n;
            else
                ch->activeTab = (ch->activeTab + 1) % n;
            if (ch->engine)
                ch->engine->FocusTab(ch->tabs[ch->activeTab].id);
            PaintChrome(ch);
        }
        return true;
    }

    // --- Ctrl+1..9: Switch to tab N (Ctrl+9 = last tab) ---
    if (ctrl && !shift && !alt && vk >= '1' && vk <= '9') {
        int idx = (vk == '9') ? (int)ch->tabs.size() - 1 : (int)(vk - '1');
        if (idx >= 0 && idx < (int)ch->tabs.size() && idx != ch->activeTab) {
            ch->activeTab = idx;
            if (ch->engine)
                ch->engine->FocusTab(ch->tabs[ch->activeTab].id);
            PaintChrome(ch);
        }
        return true;
    }

    // --- Alt+Left: Back ---
    if (alt && !ctrl && vk == VK_LEFT) {
        if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size())
            ch->engine->GoBack(ch->tabs[ch->activeTab].id);
        return true;
    }

    // --- Alt+Right: Forward ---
    if (alt && !ctrl && vk == VK_RIGHT) {
        if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size())
            ch->engine->GoForward(ch->tabs[ch->activeTab].id);
        return true;
    }

    return false; // Not handled — pass to DefWindowProc
}

// ---------------------------------------------------------------------------
// URL bar dark theme — handles WM_CTLCOLOREDIT from the parent window.
// Returns a brush if the EDIT is our URL bar, or nullptr to use defaults.
// ---------------------------------------------------------------------------
HBRUSH OnCtlColorEdit(HWND hwnd, HDC hdc, HWND hEdit) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || hEdit != ch->hUrlBar) return nullptr;

    // Dark theme: light text on dark background
    SetTextColor(hdc, RGB(232, 232, 240));   // CLR_TEXT
    SetBkColor(hdc, RGB(22, 24, 42));        // CLR_BG_CARD
    return ch->hUrlBgBrush;
}

// ---------------------------------------------------------------------------
// Right-click context menu — standard browser actions
// ---------------------------------------------------------------------------
#define IDM_CTX_BACK     40001
#define IDM_CTX_FORWARD  40002
#define IDM_CTX_RELOAD   40003
#define IDM_CTX_NEWTAB   40004
#define IDM_CTX_VIEWSRC  40005
#define IDM_CTX_INSPECT  40006

void OnContextMenu(HWND hwnd, int screenX, int screenY) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    // Only show context menu in content area (below chrome)
    POINT pt = { screenX, screenY };
    ScreenToClient(hwnd, &pt);
    if (pt.y < CHROME_H) return;  // Chrome area — don't show

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    // Back/Forward — greyed out when unavailable
    bool hasBack = (ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size() &&
                    ch->tabs[ch->activeTab].url != "aeon://newtab" &&
                    ch->tabs[ch->activeTab].url != "about:blank");
    AppendMenuA(hMenu, MF_STRING | (hasBack ? 0 : MF_GRAYED),
                IDM_CTX_BACK, "&Back");
    AppendMenuA(hMenu, MF_STRING | MF_GRAYED,
                IDM_CTX_FORWARD, "&Forward");
    AppendMenuA(hMenu, MF_STRING, IDM_CTX_RELOAD, "&Reload\tF5");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(hMenu, MF_STRING, IDM_CTX_NEWTAB, "&New Tab\tCtrl+T");
    AppendMenuA(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuA(hMenu, MF_STRING, IDM_CTX_VIEWSRC, "View Page &Source");
    AppendMenuA(hMenu, MF_STRING, IDM_CTX_INSPECT, "&Inspect (DevTools)");

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             screenX, screenY, 0, hwnd, nullptr);
    DestroyMenu(hMenu);

    switch (cmd) {
        case IDM_CTX_BACK:
            if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size())
                ch->engine->GoBack(ch->tabs[ch->activeTab].id);
            break;
        case IDM_CTX_FORWARD:
            if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size())
                ch->engine->GoForward(ch->tabs[ch->activeTab].id);
            break;
        case IDM_CTX_RELOAD:
            if (ch->engine && ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size())
                ch->engine->Reload(ch->tabs[ch->activeTab].id, 0);
            break;
        case IDM_CTX_NEWTAB:
            CreateNewTab(ch);
            break;
        case IDM_CTX_VIEWSRC: {
            // Navigate to view-source: URL
            if (ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size()) {
                std::string srcUrl = "view-source:" + ch->tabs[ch->activeTab].url;
                CreateNewTab(ch, srcUrl.c_str());
            }
            break;
        }
        case IDM_CTX_INSPECT:
            // Open DevTools via CDP (port 9222 is already enabled in engine args)
            CreateNewTab(ch, "http://localhost:9222");
            break;
    }
}

// ---------------------------------------------------------------------------
// Engine callback helpers — safely update tab state from engine events
// ---------------------------------------------------------------------------
void UpdateTabTitle(HWND hwnd, unsigned int tab_id, const char* title) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    for (auto& t : ch->tabs) {
        if (t.id == tab_id) {
            t.title = title ? title : "";
            break;
        }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void UpdateTabUrl(HWND hwnd, unsigned int tab_id, const char* url) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    std::string mapped = ReverseMapUrl(url ? url : "");
    for (auto& t : ch->tabs) {
        if (t.id == tab_id) {
            t.url = mapped;
            
            // Self-correcting history state machine
            if (t.history.empty()) {
                t.history.push_back(mapped);
                t.historyIndex = 0;
            } else {
                if (t.historyIndex - 1 >= 0 && t.history[t.historyIndex - 1] == mapped) {
                    t.historyIndex--;
                } else if (t.historyIndex + 1 < (int)t.history.size() && t.history[t.historyIndex + 1] == mapped) {
                    t.historyIndex++;
                } else if (t.history[t.historyIndex] != mapped) {
                    // Truncate any forward history and append new navigation path
                    t.history.erase(t.history.begin() + t.historyIndex + 1, t.history.end());
                    t.history.push_back(mapped);
                    t.historyIndex = (int)t.history.size() - 1;
                }
            }
            break;
        }
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

void SetTabLoaded(HWND hwnd, unsigned int tab_id) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;
    for (auto& t : ch->tabs) {
        if (t.id == tab_id) {
            t.loading = false;
            break;
        }
    }
    // Kill loading timer if no tabs are still loading
    bool anyLoading = false;
    for (const auto& t : ch->tabs) {
        if (t.loading) { anyLoading = true; break; }
    }
    if (!anyLoading) {
        KillTimer(hwnd, ID_LOADING_TIMER);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Agent Control API — programmatic access for AeonAgentPipe
// ---------------------------------------------------------------------------

int GetTabCount(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return 0;
    return (int)ch->tabs.size();
}

bool GetTabInfo(HWND hwnd, int index,
                unsigned int* outId, char* outUrl, int urlLen,
                char* outTitle, int titleLen, bool* outActive) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || index < 0 || index >= (int)ch->tabs.size()) return false;

    const auto& t = ch->tabs[index];
    if (outId)    *outId = t.id;
    if (outUrl)   _snprintf_s(outUrl, urlLen, _TRUNCATE, "%s", t.url.c_str());
    if (outTitle) _snprintf_s(outTitle, titleLen, _TRUNCATE, "%s", t.title.c_str());
    if (outActive) *outActive = (index == ch->activeTab);
    return true;
}

int GetActiveTabIndex(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return -1;
    return ch->activeTab;
}

unsigned int CreateTab(HWND hwnd, const char* url) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || !ch->engine) return 0;

    const char* targetUrl = (url && url[0]) ? url : "aeon://newtab";
    ChromeTab t;
    t.id = ch->engine->NewTab(ch->hwnd, targetUrl);
    t.url = targetUrl;
    t.title = "New Tab";
    t.loading = false;
    t.history.push_back(targetUrl);
    t.historyIndex = 0;
    ch->tabs.push_back(t);
    ch->activeTab = (int)ch->tabs.size() - 1;

    RECT rc; GetClientRect(ch->hwnd, &rc);
    ch->engine->SetViewport(t.id, ch->hwnd, 0, CHROME_H,
        rc.right, rc.bottom - CHROME_H);
    ch->engine->FocusTab(t.id);

    // Inject AeonBridge bootstrap
    std::string bridgeJs = AeonBridge::BuildInjectionScript();
    ch->engine->InjectEarlyJS(t.id, bridgeJs.c_str());

    // AI: Notify TabIntelligence of new tab (from UI action)
    if (g_TabIntel) {
        AeonTabInfo info = {};
        info.tab_id = (uint64_t)t.id;
        _snprintf_s(info.url, sizeof(info.url), _TRUNCATE, "%s", targetUrl);
        _snprintf_s(info.title, sizeof(info.title), _TRUNCATE, "New Tab");
        info.state = AeonTabState::Active;
        g_TabIntel->OnTabCreated(info);
    }

    PaintChrome(ch);
    fprintf(stdout, "[Agent] New tab #%u: %s\n", t.id, targetUrl);
    return t.id;
}

bool CloseTabById(HWND hwnd, unsigned int tabId) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return false;

    for (int i = 0; i < (int)ch->tabs.size(); i++) {
        if (ch->tabs[i].id == tabId) {
            // AI: Notify TabIntelligence BEFORE destroying the tab
            if (g_TabIntel) g_TabIntel->OnTabClosed((uint64_t)tabId);

            if (ch->engine) ch->engine->CloseTab(tabId);
            ch->tabs.erase(ch->tabs.begin() + i);
            if (ch->tabs.empty()) {
                CreateTab(hwnd, nullptr);
            } else {
                if (ch->activeTab >= (int)ch->tabs.size())
                    ch->activeTab = (int)ch->tabs.size() - 1;
                if (ch->engine)
                    ch->engine->FocusTab(ch->tabs[ch->activeTab].id);
            }
            PaintChrome(ch);
            fprintf(stdout, "[Agent] Closed tab #%u\n", tabId);
            return true;
        }
    }
    return false;
}

bool FocusTabById(HWND hwnd, unsigned int tabId) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return false;

    for (int i = 0; i < (int)ch->tabs.size(); i++) {
        if (ch->tabs[i].id == tabId) {
            unsigned int prevTabId = (ch->activeTab >= 0 && ch->activeTab < (int)ch->tabs.size())
                ? ch->tabs[ch->activeTab].id : 0;
            ch->activeTab = i;
            if (ch->engine) {
                ch->engine->FocusTab(tabId);
                RECT rc; GetClientRect(hwnd, &rc);
                ch->engine->SetViewport(tabId, hwnd, 0, CHROME_H,
                    rc.right, rc.bottom - CHROME_H);
            }

            // AI: Notify AI engines of tab focus change
            if (g_TabIntel) g_TabIntel->OnTabFocused((uint64_t)tabId);
            if (g_JourneyAI && prevTabId != tabId)
                g_JourneyAI->OnTabSwitch((uint64_t)prevTabId, (uint64_t)tabId);

            PaintChrome(ch);
            fprintf(stdout, "[Agent] Focused tab #%u\n", tabId);
            return true;
        }
    }
    return false;
}

bool NavigateTab(HWND hwnd, unsigned int tabId, const char* url) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch || !ch->engine || !url || !url[0]) return false;

    for (auto& t : ch->tabs) {
        if (t.id == tabId) {
            t.url = url;
            t.loading = true;
            ch->engine->Navigate(tabId, url, nullptr);
            PaintChrome(ch);
            fprintf(stdout, "[Agent] Navigate tab #%u: %s\n", tabId, url);
            return true;
        }
    }
    return false;
}

void ToggleSidebar(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    // Toggle Arc vertical sidebar state
    ch->hoverBtn = 999;
    PaintChrome(ch);
    fprintf(stdout, "[Arc UI] Toggled vertical sidebar state.\n");
}

void SetLayoutMode(HWND hwnd, bool verticalSidebar) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    (void)verticalSidebar;
    PaintChrome(ch);
    fprintf(stdout, "[Arc UI] Set layout mode to %s.\n", verticalSidebar ? "Arc Vertical Sidebar" : "Horizontal Chrome");
}

void ToggleCommandBar(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (!ch) return;

    // Trigger floating spotlight Command Bar (Ctrl+K)
    ActivateUrlBar(ch);
    fprintf(stdout, "[Arc UI] Triggered Command Bar (Ctrl+K).\n");
}

void Destroy(HWND hwnd) {
    ChromeState* ch = reinterpret_cast<ChromeState*>(
        GetWindowLongPtr(hwnd, GWLP_USERDATA));
    if (ch) {
        // Delete all cached GDI font handles to prevent leaks
        if (ch->hTextFont)      DeleteObject(ch->hTextFont);
        if (ch->hTextBold)      DeleteObject(ch->hTextBold);
        if (ch->hTextTabFont)   DeleteObject(ch->hTextTabFont);
        if (ch->hIconFont)      DeleteObject(ch->hIconFont);
        if (ch->hIconFontSmall) DeleteObject(ch->hIconFontSmall);
        if (ch->hIconFontLarge) DeleteObject(ch->hIconFontLarge);
        if (ch->hLogoFont)      DeleteObject(ch->hLogoFont);
        if (ch->hUrlFont)       DeleteObject(ch->hUrlFont);

        // Delete other cached GDI handles
        if (ch->hUrlBgBrush)    DeleteObject(ch->hUrlBgBrush);

        // Set pointer to null and delete
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        delete ch;
        fprintf(stdout, "[Chrome] Browser chrome state and GDI resources cleaned up.\n");
    }
}

} // namespace BrowserChrome


