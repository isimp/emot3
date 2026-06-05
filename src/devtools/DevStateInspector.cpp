#include "DevStateInspector.h"

#ifdef EMOT3_DEVTOOLS

#include "imgui/imgui.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Section {
    const char* title;
    void      (*emit)();
};

// Function-local static registry - safe to populate from the static
// initializers of any translation unit (constructed on first registration).
std::vector<Section>& Registry() {
    static std::vector<Section> r;
    return r;
}

}  // namespace

DevStateRegistrar::DevStateRegistrar(const char* title, void (*emit)()) {
    Registry().push_back({ title, emit });
}

void DevStateRow(const char* label, const char* fmt, ...) {
    char val[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(val, sizeof(val), fmt, ap);
    va_end(ap);
    // Label in the dim left column, value aligned at a fixed offset so rows
    // line up despite the proportional font.
    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(170.0f);
    ImGui::TextUnformatted(val);
}

namespace devstate {

bool& Enabled() { static bool b = false; return b; }

void Render() {
    if (!Enabled()) return;
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (ImGui::Begin("emot3 state##devstate", &Enabled(),
                     ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoNav)) {
        if (Registry().empty()) {
            ImGui::TextDisabled("no state sections registered");
        }
        int i = 0;
        for (const Section& s : Registry()) {
            // "title##devstate_<i>" keeps each header's id unique/stable.
            std::string id = std::string(s.title) + "##devstate_" + std::to_string(i++);
            if (ImGui::CollapsingHeader(id.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
                s.emit();
        }
    }
    ImGui::End();
}

}  // namespace devstate

#endif  // EMOT3_DEVTOOLS
