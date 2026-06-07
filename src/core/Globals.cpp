#include "Globals.h"

AddonAPI*      APIDefs    = nullptr;
NexusLinkData* NexusLink  = nullptr;
Mumble::Data*  MumbleLink = nullptr;

HWND        g_GameHwnd       = nullptr;
std::string g_SettingsPath;
std::string g_EmotesJsonPath;
std::string g_MeMotesJsonPath;
std::string g_IconsDir;
std::string g_PresetsDir;

bool                  g_EmotesDirty         = false;
std::atomic<uint64_t> g_EmoteCatalogVersion { 0 };
std::atomic<uint64_t> g_MeMotesVersion      { 0 };

bool                     g_PromptNewBundledEmotes = false;
std::vector<std::string> g_NewBundledEmoteIds;

std::atomic<bool> g_Unloading       { false };
std::atomic<int>  g_InflightWorkers { 0 };

std::vector<std::pair<ImVec2, ImVec2>> g_QbIconRects;

float g_QbWinX = 0.f, g_QbWinY = 0.f, g_QbWinW = 0.f, g_QbWinH = 0.f;
bool  g_QbGeometryValid = false;

bool  g_QbApplyGeometry = false;
float g_QbApplyX = 0.f, g_QbApplyY = 0.f, g_QbApplyW = 0.f, g_QbApplyH = 0.f;

float g_QbStepX = 0.f, g_QbStepY = 0.f;
int   g_QbCols = 0, g_QbRows = 0;
bool  g_QbOverflow = false;
float g_QbMaxScrollX = 0.f, g_QbMaxScrollY = 0.f;

#ifdef EMOT3_DEVTOOLS
#include "DevStateInspector.h"
// Runtime state inspector section: the live Quickbar geometry + grid metrics
// these globals carry (the inputs to the fit/snap/scroll math). Self-registers
// via the Layer-2 standard (DevStateInspector.h).
static DevStateRegistrar s_qbGridSection("Quickbar grid", [] {
    DevStateRow("window x,y",     "%.0f, %.0f", g_QbWinX, g_QbWinY);
    DevStateRow("window w,h",     "%.0f x %.0f", g_QbWinW, g_QbWinH);
    DevStateRow("geometry valid", "%s", g_QbGeometryValid ? "yes" : "no");
    DevStateRow("step x,y",       "%.1f x %.1f", g_QbStepX, g_QbStepY);
    DevStateRow("cols,rows",      "%d x %d", g_QbCols, g_QbRows);
    DevStateRow("overflow",       "%s", g_QbOverflow ? "yes" : "no");
    DevStateRow("max scroll x,y", "%.1f, %.1f", g_QbMaxScrollX, g_QbMaxScrollY);
});
#endif  // EMOT3_DEVTOOLS
