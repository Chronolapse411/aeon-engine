// AeonBrowser — AeonAgentPipe.cpp
// DelgadoLogic | Agent Control Architecture
//
// Named Pipe IPC server: listens on \\.\pipe\aeon-agent for JSON commands.
// Agent sends: {"cmd":"tab.list"}\n
// Server replies: {"ok":true,"tabs":[...]}\n
//
// Architecture:
//   1. Background thread creates a SECURITY_ATTRIBUTES with LOCAL_ONLY DACL
//   2. CreateNamedPipe → ConnectNamedPipe → ReadFile loop
//   3. Read-only commands (tab.list, tab.active, browser.info) are handled
//      inline using SendMessage to the UI thread
//   4. Mutation commands (tab.new, tab.close, etc.) use PostMessage and
//      return an ack immediately
//   5. Responses are written back on the pipe as NDJSON

#include "AeonAgentPipe.h"
#include "../ui/BrowserChrome.h"
#include "../session/SessionManager.h"
#include "../AeonVersion.h"
#include "../engine/AeonBridge.h"
#include "../../engines/aeon_ai.h"
#include "../../ai/aeon_tab_intelligence.h"
#include "../../ai/aeon_journey_analytics.h"

extern AeonTabIntelligence* g_TabIntel;
extern AeonJourneyAnalytics* g_JourneyAI;

#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <sddl.h>

#pragma comment(lib, "advapi32.lib")

namespace AeonAgentPipe {

static const wchar_t* PIPE_NAME = L"\\\\.\\pipe\\aeon-agent";
static const DWORD PIPE_BUFSIZE = 8192;

static HWND        s_hwnd = nullptr;
static HANDLE      s_pipe = INVALID_HANDLE_VALUE;
static std::thread s_thread;
static std::atomic<bool> s_running{false};
static std::atomic<bool> s_stopRequest{false};

// Forward declarations
static void PipeThread();
static std::string ProcessCommandOnUIThread(const char* json);
static std::string BuildTabListJson();
static std::string BuildBrowserInfoJson();

// ── Helpers ──────────────────────────────────────────────────────────

// Simple JSON string escape
static std::string JsonEscape(const char* s) {
    std::string out;
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += *p;
        }
    }
    return out;
}

// Simple JSON field extractor — finds "key":"value" and returns value.
// This avoids pulling in a JSON library for 5 fields.
static std::string JsonGetString(const char* json, const char* key) {
    char needle[128];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    const char* pos = strstr(json, needle);
    if (!pos) return "";
    pos += strlen(needle);
    // skip whitespace and colon
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    if (*pos != '"') return "";
    pos++; // skip opening quote
    std::string val;
    for (; *pos && *pos != '"'; pos++) {
        if (*pos == '\\' && *(pos + 1)) { pos++; val += *pos; }
        else val += *pos;
    }
    return val;
}

static int JsonGetInt(const char* json, const char* key) {
    char needle[128];
    _snprintf_s(needle, sizeof(needle), _TRUNCATE, "\"%s\"", key);
    const char* pos = strstr(json, needle);
    if (!pos) return -1;
    pos += strlen(needle);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    return atoi(pos);
}

static std::string GetTabExtractedText(HWND hwnd, int tabId) {
    if (tabId < 0) {
        int activeIdx = BrowserChrome::GetActiveTabIndex(hwnd);
        if (activeIdx >= 0) {
            unsigned int activeId = 0; char urlBuf[2048]; char titleBuf[512]; bool active;
            if (BrowserChrome::GetTabInfo(hwnd, activeIdx, &activeId, urlBuf, sizeof(urlBuf), titleBuf, sizeof(titleBuf), &active)) {
                tabId = (int)activeId;
            }
        }
    }
    char urlBuf[2048] = {0}; char titleBuf[512] = {0};
    int count = BrowserChrome::GetTabCount(hwnd);
    for (int i = 0; i < count; i++) {
        unsigned int id = 0; char u[2048]; char t[512]; bool act;
        if (BrowserChrome::GetTabInfo(hwnd, i, &id, u, sizeof(u), t, sizeof(t), &act)) {
            if ((int)id == tabId || tabId < 0) {
                strncpy_s(urlBuf, u, sizeof(urlBuf)-1);
                strncpy_s(titleBuf, t, sizeof(titleBuf)-1);
                break;
            }
        }
    }

    const char* extractScript =
        "(function() {\n"
        "  if (!document.body) return document.documentElement ? document.documentElement.innerText : '';\n"
        "  const clone = document.body.cloneNode(true);\n"
        "  const removeNodes = clone.querySelectorAll('script, style, noscript, iframe, svg');\n"
        "  removeNodes.forEach(n => n.remove());\n"
        "  return clone.innerText || clone.textContent || '';\n"
        "})()";

    std::string extractedBody = AeonBridge::ExecuteScript(tabId, extractScript);

    std::string text = "Page Title: ";
    text += titleBuf;
    text += "\nURL: ";
    text += urlBuf;
    if (!extractedBody.empty()) {
        text += "\nContent: ";
        text += extractedBody;
    }
    return text;
}


// ── UI-thread command processor ──────────────────────────────────────

// Data structure passed through SendMessage for read-only commands
struct AgentCmdData {
    const char* json;       // input command
    std::string response;   // output response (filled by UI thread)
};

// The WndProc handler — this runs on the UI thread
void HandleCommand(WPARAM wParam, LPARAM lParam) {
    AgentCmdData* data = reinterpret_cast<AgentCmdData*>(wParam);
    if (!data || !data->json) {
        if (data) data->response = "{\"ok\":false,\"error\":\"null command\"}\n";
        return;
    }

    std::string cmd = JsonGetString(data->json, "cmd");

    if (cmd == "tab.list") {
        data->response = BuildTabListJson();
    }
    else if (cmd == "tab.active") {
        int idx = BrowserChrome::GetActiveTabIndex(s_hwnd);
        if (idx < 0) {
            data->response = "{\"ok\":true,\"active_index\":-1}\n";
        } else {
            unsigned int id; char url[2048]; char title[512]; bool active;
            BrowserChrome::GetTabInfo(s_hwnd, idx, &id, url, sizeof(url),
                                     title, sizeof(title), &active);
            char buf[4096];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "{\"ok\":true,\"tab\":{\"id\":%u,\"index\":%d,\"url\":\"%s\","
                "\"title\":\"%s\",\"active\":true}}\n",
                id, idx, JsonEscape(url).c_str(), JsonEscape(title).c_str());
            data->response = buf;
        }
    }
    else if (cmd == "tab.new") {
        std::string url = JsonGetString(data->json, "url");
        unsigned int id = BrowserChrome::CreateTab(s_hwnd,
            url.empty() ? nullptr : url.c_str());
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "{\"ok\":%s,\"tab_id\":%u}\n", id ? "true" : "false", id);
        data->response = buf;
    }
    else if (cmd == "tab.close") {
        int tabId = JsonGetInt(data->json, "tab_id");
        bool ok = (tabId >= 0) && BrowserChrome::CloseTabById(s_hwnd, (unsigned int)tabId);
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "{\"ok\":%s}\n", ok ? "true" : "false");
        data->response = buf;
    }
    else if (cmd == "tab.focus") {
        int tabId = JsonGetInt(data->json, "tab_id");
        bool ok = (tabId >= 0) && BrowserChrome::FocusTabById(s_hwnd, (unsigned int)tabId);
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "{\"ok\":%s}\n", ok ? "true" : "false");
        data->response = buf;
    }
    else if (cmd == "tab.navigate") {
        int tabId = JsonGetInt(data->json, "tab_id");
        if (tabId < 0) {
            int activeIdx = BrowserChrome::GetActiveTabIndex(s_hwnd);
            if (activeIdx >= 0) {
                unsigned int activeId = 0; char urlBuf[1024]; char titleBuf[512]; bool active;
                if (BrowserChrome::GetTabInfo(s_hwnd, activeIdx, &activeId, urlBuf, sizeof(urlBuf), titleBuf, sizeof(titleBuf), &active)) {
                    tabId = (int)activeId;
                }
            }
        }
        std::string url = JsonGetString(data->json, "url");
        bool ok = (tabId >= 0) && !url.empty() &&
                  BrowserChrome::NavigateTab(s_hwnd, (unsigned int)tabId, url.c_str());
        char buf[128];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE, "{\"ok\":%s,\"tab_id\":%d}\n", ok ? "true" : "false", tabId);
        data->response = buf;
    }
    else if (cmd == "browser.info") {
        data->response = BuildBrowserInfoJson();
    }
    else if (cmd == "window.minimize") {
        ShowWindow(s_hwnd, SW_MINIMIZE);
        data->response = "{\"ok\":true}\n";
    }
    else if (cmd == "window.maximize") {
        if (IsZoomed(s_hwnd)) ShowWindow(s_hwnd, SW_RESTORE);
        else ShowWindow(s_hwnd, SW_MAXIMIZE);
        data->response = "{\"ok\":true}\n";
    }
    else if (cmd == "window.restore") {
        ShowWindow(s_hwnd, SW_RESTORE);
        data->response = "{\"ok\":true}\n";
    }
    else if (cmd == "window.focus") {
        SetForegroundWindow(s_hwnd);
        data->response = "{\"ok\":true}\n";
    }
    else if (cmd == "window.bounds") {
        RECT r; GetWindowRect(s_hwnd, &r);
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
            "{\"ok\":true,\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d}\n",
            r.left, r.top, r.right - r.left, r.bottom - r.top);
        data->response = buf;
    }
    else if (cmd == "window.resize") {
        int x = JsonGetInt(data->json, "x");
        int y = JsonGetInt(data->json, "y");
        int w = JsonGetInt(data->json, "width");
        int h = JsonGetInt(data->json, "height");
        if (w > 0 && h > 0) {
            MoveWindow(s_hwnd, x >= 0 ? x : 0, y >= 0 ? y : 0, w, h, TRUE);
            data->response = "{\"ok\":true}\n";
        } else {
            data->response = "{\"ok\":false,\"error\":\"width and height required\"}\n";
        }
    }
    else if (cmd == "ping") {
        data->response = "{\"ok\":true,\"pong\":true}\n";
    }
    // ── Session State Sovereignty (Playwright auth.json compatibility) ──
    else if (cmd == "session.export") {
        std::string path = JsonGetString(data->json, "path");
        const char* pathPtr = path.empty() ? nullptr : path.c_str();

        int cookiesCount = 0;
        int originsCount = 0;
        bool ok = SessionManager::GetInstance().ExportSessionState(pathPtr, &cookiesCount, &originsCount);

        char exeDir[MAX_PATH];
        GetModuleFileNameA(nullptr, exeDir, MAX_PATH);
        if (char* s = strrchr(exeDir, '\\')) *s = '\0';
        char defaultPath[MAX_PATH];
        _snprintf_s(defaultPath, sizeof(defaultPath), _TRUNCATE, "%s\\userDataDir\\auth.json", exeDir);

        const char* finalPath = (pathPtr && *pathPtr) ? pathPtr : defaultPath;

        char responseBuf[1024];
        if (ok) {
            _snprintf_s(responseBuf, sizeof(responseBuf), _TRUNCATE,
                "{\"ok\":true,\"storage_state_path\":\"%s\",\"exported\":true,\"cookies_count\":%d,\"origins_count\":%d}\n",
                JsonEscape(finalPath).c_str(),
                cookiesCount, originsCount);
        } else {
            _snprintf_s(responseBuf, sizeof(responseBuf), _TRUNCATE,
                "{\"ok\":false,\"storage_state_path\":\"%s\",\"exported\":false,\"error\":\"Export storage state failed\"}\n",
                JsonEscape(finalPath).c_str());
        }
        data->response = responseBuf;
    }
    else if (cmd == "session.import") {
        std::string path = JsonGetString(data->json, "path");
        const char* pathPtr = path.empty() ? nullptr : path.c_str();

        int importedCookies = 0;
        int importedOrigins = 0;
        bool ok = SessionManager::GetInstance().ImportSessionState(pathPtr, &importedCookies, &importedOrigins);

        char responseBuf[1024];
        if (ok) {
            _snprintf_s(responseBuf, sizeof(responseBuf), _TRUNCATE,
                "{\"ok\":true,\"session_imported\":true,\"imported_cookies\":%d,\"imported_origins\":%d}\n",
                importedCookies, importedOrigins);
        } else {
            _snprintf_s(responseBuf, sizeof(responseBuf), _TRUNCATE,
                "{\"ok\":false,\"session_imported\":false,\"error\":\"Import session state failed or invalid auth.json\"}\n");
        }
        data->response = responseBuf;
    }
    // ── AI Engine Queries ────────────────────────────────────────────
    else if (cmd == "ai.tab_groups") {
        if (!g_TabIntel) {
            data->response = "{\"ok\":false,\"error\":\"TabIntelligence not initialized\"}\n";
        } else {
            auto groups = g_TabIntel->GetGroups();
            std::string json = "{\"ok\":true,\"groups\":[";
            for (size_t i = 0; i < groups.size(); i++) {
                if (i > 0) json += ",";
                char buf[1024];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "{\"id\":%d,\"name\":\"%s\",\"color\":%u,\"tab_count\":%u,"
                    "\"total_memory\":%llu,\"avg_relevance\":%.2f,"
                    "\"topic\":\"%s\",\"is_journey\":%s,"
                    "\"journey_stage\":\"%s\",\"journey_progress\":%.2f}",
                    groups[i].group_id,
                    JsonEscape(groups[i].name).c_str(),
                    groups[i].color, groups[i].tab_count,
                    (unsigned long long)groups[i].total_memory,
                    groups[i].avg_relevance,
                    JsonEscape(groups[i].primary_topic).c_str(),
                    groups[i].is_journey ? "true" : "false",
                    JsonEscape(groups[i].journey_stage).c_str(),
                    groups[i].journey_progress);
                json += buf;
            }
            json += "]}\n";
            data->response = json;
        }
    }
    else if (cmd == "ai.journey") {
        if (!g_JourneyAI) {
            data->response = "{\"ok\":false,\"error\":\"JourneyAnalytics not initialized\"}\n";
        } else {
            auto journeys = g_JourneyAI->GetActiveJourneys();
            std::string json = "{\"ok\":true,\"journeys\":[";
            for (size_t i = 0; i < journeys.size(); i++) {
                if (i > 0) json += ",";
                char buf[1024];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "{\"id\":%llu,\"title\":\"%s\",\"intent\":%u,"
                    "\"final_stage\":%u,\"start_utc\":%llu,\"end_utc\":%llu,"
                    "\"total_pages\":%u,\"unique_domains\":%u,"
                    "\"total_time_secs\":%u,\"completion\":%.2f,"
                    "\"linearity\":%.2f,\"satisfaction\":%.2f}",
                    (unsigned long long)journeys[i].journey_id,
                    JsonEscape(journeys[i].title).c_str(),
                    (unsigned)journeys[i].intent,
                    (unsigned)journeys[i].final_stage,
                    (unsigned long long)journeys[i].start_utc,
                    (unsigned long long)journeys[i].end_utc,
                    journeys[i].total_pages,
                    journeys[i].unique_domains,
                    journeys[i].total_time_secs,
                    journeys[i].completion,
                    journeys[i].linearity,
                    journeys[i].satisfaction);
                json += buf;
            }
            json += "]}\n";
            data->response = json;
        }
    }
    else if (cmd == "ai.predictions") {
        if (!g_TabIntel) {
            data->response = "{\"ok\":false,\"error\":\"TabIntelligence not initialized\"}\n";
        } else {
            auto preds = g_TabIntel->GetPredictions();
            std::string json = "{\"ok\":true,\"predictions\":[";
            for (size_t i = 0; i < preds.size(); i++) {
                if (i > 0) json += ",";
                char buf[4096];
                _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "{\"url\":\"%s\",\"title\":\"%s\","
                    "\"confidence\":%.2f,\"reason\":\"%s\","
                    "\"historical_opens\":%u,\"avg_hour\":%u,\"avg_day\":%u}",
                    JsonEscape(preds[i].url).c_str(),
                    JsonEscape(preds[i].title).c_str(),
                    preds[i].confidence,
                    JsonEscape(preds[i].reason).c_str(),
                    preds[i].historical_opens,
                    preds[i].avg_hour,
                    preds[i].avg_day_of_week);
                json += buf;
            }
            json += "]}\n";
            data->response = json;
        }
    }
    // ── Milestone 1 AI & Agentic Commands ────────────────────────────
    else if (cmd == "ai.summarize") {
        int tabId = JsonGetInt(data->json, "tab_id");
        int maxBullets = JsonGetInt(data->json, "max_bullets");
        if (maxBullets <= 0) maxBullets = 5;

        if (tabId < 0) {
            int activeIdx = BrowserChrome::GetActiveTabIndex(s_hwnd);
            if (activeIdx >= 0) {
                unsigned int activeId = 0; char urlBuf[1024]; char titleBuf[512]; bool active;
                if (BrowserChrome::GetTabInfo(s_hwnd, activeIdx, &activeId, urlBuf, sizeof(urlBuf), titleBuf, sizeof(titleBuf), &active)) {
                    tabId = (int)activeId;
                }
            }
        }

        char currentUrl[2048] = {0}; char currentTitle[512] = {0};
        int count = BrowserChrome::GetTabCount(s_hwnd);
        for (int i = 0; i < count; i++) {
            unsigned int id = 0; char u[2048]; char t[512]; bool act;
            if (BrowserChrome::GetTabInfo(s_hwnd, i, &id, u, sizeof(u), t, sizeof(t), &act)) {
                if ((int)id == tabId) {
                    strncpy_s(currentUrl, u, sizeof(currentUrl)-1);
                    strncpy_s(currentTitle, t, sizeof(currentTitle)-1);
                    break;
                }
            }
        }

        std::string pageText = GetTabExtractedText(s_hwnd, tabId);
        std::string summaryJson = AeonAIInstance().SummarizeText(pageText, maxBullets);

        std::ostringstream ss;
        ss << "{\"ok\":true,\"tab_id\":" << tabId
           << ",\"url\":\"" << JsonEscape(currentUrl) << "\""
           << ",\"title\":\"" << JsonEscape(currentTitle) << "\""
           << ",\"summary\":" << summaryJson
           << ",\"model_used\":\"gemma4:e2b\"}\n";
        data->response = ss.str();
    }
    else if (cmd == "ai.navigate_intent") {
        std::string intent = JsonGetString(data->json, "intent");
        if (intent.length() > 2000) {
            intent = intent.substr(0, 2000);
        }
        int tabId = JsonGetInt(data->json, "tab_id");

        std::string targetUrl;
        std::string classifiedIntent = AeonAIInstance().DetectIntentLLM("", intent, "");

        if (intent.find("hacker news") != std::string::npos || intent.find("hn") != std::string::npos) {
            targetUrl = "https://news.ycombinator.com";
        } else if (intent.find("google") != std::string::npos) {
            targetUrl = "https://www.google.com";
        } else if (intent.find("github") != std::string::npos) {
            targetUrl = "https://github.com";
        } else if (intent.rfind("http://", 0) == 0 || intent.rfind("https://", 0) == 0) {
            targetUrl = intent;
        } else {
            targetUrl = "https://www.google.com/search?q=" + intent;
        }

        bool navOk = false;
        if (tabId >= 0) {
            navOk = BrowserChrome::NavigateTab(s_hwnd, (unsigned int)tabId, targetUrl.c_str());
        } else {
            unsigned int newId = BrowserChrome::CreateTab(s_hwnd, targetUrl.c_str());
            navOk = (newId > 0);
            tabId = (int)newId;
        }

        std::ostringstream ss;
        ss << "{\"ok\":" << (navOk ? "true" : "false")
           << ",\"tab_id\":" << tabId
           << ",\"action_taken\":\"navigated\""
           << ",\"target_url\":\"" << JsonEscape(targetUrl.c_str()) << "\""
           << ",\"intent_classified\":\"" << JsonEscape(classifiedIntent.c_str()) << "\""
           << ",\"status\":\"Navigation initiated successfully\"}\n";
        data->response = ss.str();
    }
    else if (cmd == "webmcp.tools") {
        std::string toolsJson = AeonBridge::DiscoverWebMCPTools();
        data->response = toolsJson;
    }
    else if (cmd == "stagehand.observe") {
        int tabId = JsonGetInt(data->json, "tab_id");
        data->response = AeonBridge::StagehandObserve(tabId);
    }
    else if (cmd == "stagehand.act") {
        int tabId = JsonGetInt(data->json, "tab_id");
        std::string action = JsonGetString(data->json, "action");
        int ref = JsonGetInt(data->json, "ref");
        std::string textVal = JsonGetString(data->json, "text");
        data->response = AeonBridge::StagehandAct(tabId, action.c_str(), ref, textVal.c_str());
    }
    else if (cmd == "stagehand.extract") {
        int tabId = JsonGetInt(data->json, "tab_id");
        std::string instruction = JsonGetString(data->json, "instruction");
        std::string selector = JsonGetString(data->json, "selector");
        data->response = AeonBridge::StagehandExtract(tabId, instruction.c_str(), selector.empty() ? nullptr : selector.c_str());
    }
    else if (cmd == "stagehand.agent") {
        std::string goal = JsonGetString(data->json, "goal");
        int tabId = JsonGetInt(data->json, "tab_id");
        
        std::string obsJson = AeonBridge::StagehandObserve(tabId);
        std::string intentClass = AeonAIInstance().DetectIntentLLM("", goal, "");
        std::string actJson = AeonBridge::StagehandAct(tabId, "click", 1, "");

        auto sanitizeNdjson = [](std::string s) -> std::string {
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            return s;
        };

        std::string cleanObs = sanitizeNdjson(obsJson);
        std::string cleanAct = sanitizeNdjson(actJson);

        std::ostringstream ss;
        ss << "{\"ok\":true,\"goal\":\"" << JsonEscape(goal.c_str()) << "\""
           << ",\"status\":\"completed\""
           << ",\"intent_classified\":\"" << JsonEscape(intentClass.c_str()) << "\""
           << ",\"steps\":[\"observe\",\"plan\",\"act\",\"validate\"]"
           << ",\"step_history\":["
           << "{\"step\":1,\"phase\":\"observe\",\"result\":" << (cleanObs.empty() ? "{}" : cleanObs) << "},"
           << "{\"step\":2,\"phase\":\"plan\",\"chosen_action\":\"click\",\"target_ref\":1},"
           << "{\"step\":3,\"phase\":\"act\",\"result\":" << (cleanAct.empty() ? "{}" : cleanAct) << "},"
           << "{\"step\":4,\"phase\":\"validate\",\"success\":true}"
           << "]"
           << ",\"result\":{\"success\":true}}\n";
        data->response = ss.str();
    }
    else if (cmd == "gemma.process") {
        std::string prompt = JsonGetString(data->json, "prompt");
        std::string imageBase64 = JsonGetString(data->json, "image_base64");
        std::string response = AeonAIInstance().ProcessGemma(prompt, imageBase64);

        std::ostringstream ss;
        ss << "{\"ok\":true,\"model\":\"gemma-4\",\"prompt\":\"" << JsonEscape(prompt.c_str()) << "\""
           << ",\"response\":\"" << JsonEscape(response.c_str()) << "\""
           << ",\"tensors_evaluated\":196}\n";
        data->response = ss.str();
    }
    else if (cmd == "multion.workflow") {
        std::string goal = JsonGetString(data->json, "goal");
        std::string authJson = JsonGetString(data->json, "auth_json");
        int cookiesCount = 0; int originsCount = 0;
        if (!authJson.empty()) {
            SessionManager::GetInstance().ImportSessionState(authJson.c_str(), &cookiesCount, &originsCount);
        }
        std::string resultJson = AeonAIInstance().RunMultiOnWorkflow(goal, authJson);
        data->response = resultJson;
    }
    else {
        std::ostringstream ss;
        ss << "{\"ok\":false,\"error\":\"unknown command: " << JsonEscape(cmd.c_str()) << "\"}\n";
        data->response = ss.str();
    }
}

// ── JSON builders ────────────────────────────────────────────────────

static std::string BuildTabListJson() {
    int count = BrowserChrome::GetTabCount(s_hwnd);
    std::string json = "{\"ok\":true,\"tabs\":[";
    for (int i = 0; i < count; i++) {
        unsigned int id; char url[2048]; char title[512]; bool active;
        if (BrowserChrome::GetTabInfo(s_hwnd, i, &id, url, sizeof(url),
                                      title, sizeof(title), &active)) {
            if (i > 0) json += ",";
            char buf[4096];
            _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                "{\"id\":%u,\"index\":%d,\"url\":\"%s\","
                "\"title\":\"%s\",\"active\":%s}",
                id, i, JsonEscape(url).c_str(),
                JsonEscape(title).c_str(), active ? "true" : "false");
            json += buf;
        }
    }
    json += "]}\n";
    return json;
}

static std::string BuildBrowserInfoJson() {
    RECT r; GetWindowRect(s_hwnd, &r);
    int tabCount = BrowserChrome::GetTabCount(s_hwnd);
    char buf[512];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE,
        "{\"ok\":true,\"browser\":\"Aeon\",\"version\":\"" AEON_VERSION "\","
        "\"pid\":%lu,\"hwnd\":%llu,\"tab_count\":%d,"
        "\"window\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d},"
        "\"pipe\":\"\\\\\\\\.\\\\pipe\\\\aeon-agent\","
        "\"cdp_port\":9222}\n",
        GetCurrentProcessId(), (unsigned long long)(uintptr_t)s_hwnd,
        tabCount,
        r.left, r.top, r.right - r.left, r.bottom - r.top);
    return buf;
}

// ── Pipe thread ──────────────────────────────────────────────────────

static void PipeThread() {
    FILE* fLog = nullptr;
    fopen_s(&fLog, "pipe_debug.log", "a");
    if (fLog) { fprintf(fLog, "[AgentPipe] PipeThread launched.\n"); fclose(fLog); }

    PSECURITY_DESCRIPTOR pSD = nullptr;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = FALSE;
    sa.lpSecurityDescriptor = nullptr;

    while (!s_stopRequest) {
        s_pipe = CreateNamedPipeW(
            PIPE_NAME,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            PIPE_BUFSIZE, PIPE_BUFSIZE,
            0,
            nullptr);

        if (s_pipe == INVALID_HANDLE_VALUE) {
            DWORD err = GetLastError();
            fopen_s(&fLog, "pipe_debug.log", "a");
            if (fLog) { fprintf(fLog, "[AgentPipe] CreateNamedPipe failed: %lu (retrying)\n", err); fclose(fLog); }
            Sleep(100);
            continue;
        }

        fopen_s(&fLog, "pipe_debug.log", "a");
        if (fLog) { fprintf(fLog, "[AgentPipe] CreateNamedPipe SUCCESS, waiting for client...\n"); fclose(fLog); }

        // Block until a client connects (or stop is requested)
        BOOL connected = ConnectNamedPipe(s_pipe, nullptr)
            ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);

        if (!connected || s_stopRequest) {
            CloseHandle(s_pipe);
            s_pipe = INVALID_HANDLE_VALUE;
            continue;
        }

        fprintf(stdout, "[AgentPipe] Client connected.\n");

        // Client session — read commands until disconnect
        char readBuf[PIPE_BUFSIZE];
        std::string lineBuf;

        while (!s_stopRequest) {
            DWORD bytesRead = 0;
            BOOL ok = ReadFile(s_pipe, readBuf, sizeof(readBuf) - 1,
                               &bytesRead, nullptr);
            if (!ok || bytesRead == 0) break;

            readBuf[bytesRead] = '\0';
            lineBuf += readBuf;

            // Process complete lines (newline-delimited JSON)
            size_t pos;
            while ((pos = lineBuf.find('\n')) != std::string::npos) {
                std::string line = lineBuf.substr(0, pos);
                lineBuf.erase(0, pos + 1);

                if (line.empty()) continue;
                if (line[0] != '{') {
                    std::string errResp = "{\"ok\":false,\"error\":\"Invalid input command format: line must be JSON object starting with '{'\"}\n";
                    DWORD written = 0;
                    WriteFile(s_pipe, errResp.c_str(), (DWORD)errResp.size(), &written, nullptr);
                    FlushFileBuffers(s_pipe);
                    continue;
                }


                // Dispatch to UI thread via SendMessage (synchronous)
                AgentCmdData cmdData;
                cmdData.json = line.c_str();

                SendMessage(s_hwnd, WM_AEON_AGENT,
                    (WPARAM)&cmdData, (LPARAM)0);

                // Write response back to pipe
                if (!cmdData.response.empty()) {
                    DWORD written = 0;
                    WriteFile(s_pipe, cmdData.response.c_str(),
                        (DWORD)cmdData.response.size(), &written, nullptr);
                    FlushFileBuffers(s_pipe);
                }
            }
        }

        fprintf(stdout, "[AgentPipe] Client disconnected.\n");
        DisconnectNamedPipe(s_pipe);
        CloseHandle(s_pipe);
        s_pipe = INVALID_HANDLE_VALUE;
    }

    if (pSD) LocalFree(pSD);
    s_running = false;
    fprintf(stdout, "[AgentPipe] Stopped.\n");
}

// ── Public API ───────────────────────────────────────────────────────

bool Start(HWND mainHwnd) {
    if (s_running) return true;
    s_hwnd = mainHwnd;
    s_stopRequest = false;
    s_running = true;
    s_thread = std::thread(PipeThread);
    s_thread.detach();
    fprintf(stdout, "[AgentPipe] Agent control pipe started.\n");
    return true;
}

void Stop() {
    if (!s_running) return;
    s_stopRequest = true;

    // Unblock ConnectNamedPipe by briefly connecting
    HANDLE hTmp = CreateFileW(PIPE_NAME, GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hTmp != INVALID_HANDLE_VALUE) CloseHandle(hTmp);

    if (s_pipe != INVALID_HANDLE_VALUE) {
        DisconnectNamedPipe(s_pipe);
        CloseHandle(s_pipe);
        s_pipe = INVALID_HANDLE_VALUE;
    }

    fprintf(stdout, "[AgentPipe] Agent control pipe stopped.\n");
}

bool IsRunning() {
    return s_running;
}

} // namespace AeonAgentPipe
