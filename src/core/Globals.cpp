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
// (The dev-only "Quickbar grid" state-inspector section that used to live here
// was folded into the Quickbar diagnostics window - see QuickbarDebug.h.)
