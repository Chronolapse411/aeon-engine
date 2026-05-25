# Project: Aeon Browser UI Rebuild

## Architecture
Aeon Browser is a native Win32 C++ browser shell containing owner-drawn chrome elements built on GDI painting and compatible memory double-buffering. It wraps the WebView2/CEF engine for HTML/JS rendering and integrates directly with on-device NLP Tab Intelligence (`g_TabIntel`) and Journey Analytics (`g_JourneyAI`) systems, as well as a network sentinel (`NetworkSentinel`) for Tor routing.

```
                  ┌─────────────────────────────────┐
                  │          AeonMain.cpp           │
                  └────────────────┬────────────────┘
                                   │
                                   │ Spawns & Sets up
                                   ▼
                  ┌─────────────────────────────────┐
                  │   core/ui/BrowserChrome.cpp     │
                  └─────────┬──────────────┬────────┘
                             │              │
        Paints Chrome via   │              │ Manages tabs &
        Offscreen Memory    │              │ dispatches messages
        compatible DC       ▼              ▼
     ┌────────────────────────┐      ┌────────────────────────┐
     │   Double Buffered GDI  │      │    WebView2 Engine     │
     │      OnPaint Loop      │      │       Viewport         │
     └────────────────────────┘      └────────────┬───────────┘
                                                  │
                                                  │ Loads internal URL
                                                  ▼
      ┌────────────────────────┐      ┌────────────────────────┐
      │    AeonTabIntelligence │◄─────┤    aeon://intelligence │
      │     g_TabIntel (NLP)   │      │    aeon://journey      │
      └────────────────────────┘      └────────────┬───────────┘
                                                   │
      ┌────────────────────────┐                   │ Binds via host
      │  AeonJourneyAnalytics  │◄──────────────────┘ adapter object
      │   g_JourneyAI (Graph)  │
      └────────────────────────┘
```

## Code Layout
- `core/ui/BrowserChrome.cpp`: Main Win32 UI drawing and message dispatcher.
- `core/ui/BrowserChrome.h`: Public interface definitions for the browser chrome.
- `core/ui/AppMenu.cpp`: Custom-drawn Win32 dropdown application menu window.
- `core/ui/AppMenu.h`: Public API and structural definitions for the AppMenu popup.
- `core/engine/AeonBridge.cpp`: C++ to JS bridging and WebView2 navigation interceptions.
- `ai/aeon_tab_intelligence.h`: NLP-based Tab Intelligence clustering and auto-hibernation.
- `ai/aeon_journey_analytics.h`: directed-graph tracking of navigation history.

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | Premium Tab & Toolbar Paint | Rounded organic borders, custom hovers, smooth color-shifting animations, bottom violet active-glow underlines, glassmorphism grouping, and zero flicker painting. | none | DONE (implementer) |
| 2 | Navigation Controls Integration | Bind downloads (`aeon://downloads`), bookmarks (`aeon://bookmarks`), and Tor mode (setting persistence, `NetworkSentinel::ApplyBestStrategy` routing, visual color updates) buttons. Subclass EDIT URL control for wide characters. | M1 | DONE (implementer) |
| 3 | Three-Dot Menu Integration | Wire `AppMenu::Show` to the menu button, mapping Bookmarks, Downloads, History, and Settings within the custom dropdown to trigger active tab internal navigations. | M1, M2 | DONE (implementer) |
| 4 | AI Feature Showcase | Integrate AI Summary icon (triggers WebView2 page `aeon://intelligence`) and AI Journey Map icon (opens `aeon://journey`) onto the main toolbar. Verify C++ event hooks. | M1, M3 | DONE (implementer) |
| 5 | Dual Track Verification | Run `build.ps1 -Tier Pro`, run E2E test-ui-features.mjs, and verify stability. Perform Forensic Integrity Audit. | M4 | DONE (auditor) |

## Interface Contracts
### `BrowserChrome` ↔ `AppMenu`
- `AppMenu::Show(HWND parent, POINT anchorPt, MenuCallback callback)` displays custom dark-themed native popup menu.
- `MenuCallback` receives `ItemId` (e.g. `ItemId::History`) and initiates corresponding chrome tab navigations in C++.

### `BrowserChrome` ↔ `NetworkSentinel`
- Clicking Tor toggles `ch->settings.tor_enabled`.
- Invokes `SettingsEngine::Save(ch->settings)` to persist choices.
- Invokes `NetworkSentinel::ApplyBestStrategy()` to instantiate SOCKS5 Tor tunnels.
- Triggers `PaintChrome` to update shield visual color (active green vs inactive dim).
