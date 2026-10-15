#include "render/overlay.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

#include "common.h"
#include "game_data.h"
#include "log.h"
#include "meter/fight_log_store.h"
#include "meter/meter.h"
#include "render/input.h"
#include "settings.h"

namespace dps::render::overlay {
namespace {

// Party HUD slot colours: orange, green, blue, pink.
const ImVec4 kSlotColors[kPartySlots] = {
    {1.00f, 0.62f, 0.18f, 1.0f},
    {0.38f, 0.86f, 0.42f, 1.0f},
    {0.32f, 0.72f, 1.00f, 1.0f},
    {0.95f, 0.42f, 0.72f, 1.0f},
};
const ImVec4 kTrialRunning = {0.35f, 0.75f, 1.00f, 1.0f};
const ImVec4 kTrialEnding = {1.00f, 0.45f, 0.30f, 1.0f};
const ImVec4 kTrialBest = {1.00f, 0.85f, 0.25f, 1.0f};
constexpr float kTrialWarningSeconds = 5.0f;

// Hunter names, quest names and file names may contain '%': never use the printf
// forms of ImGui text functions with them.
void Text(const std::string& text) { ImGui::TextUnformatted(text.c_str()); }

void Disabled(const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
}

void Colored(const ImVec4& color, const std::string& text) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopStyleColor();
}

void Wrapped(const std::string& text) {
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
}

/// Right-aligns the next text inside the current table cell.
void RightAligned(const std::string& text, bool disabled = false) {
    const float width = ImGui::CalcTextSize(text.c_str()).x;
    const float available = ImGui::GetContentRegionAvail().x;
    if (available > width)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + available - width);
    if (disabled)
        Disabled(text);
    else
        Text(text);
}

void DrawPartyTable(const meter::OverlayView& view) {
    const float duration = std::max(view.elapsed, 1.0f);
    // Fixed-fit columns: the overlay window auto-sizes, so stretch columns would
    // collapse it to nothing.
    if (ImGui::BeginTable("##dps_rows", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("Hunter name").x);
        ImGui::TableSetupColumn("dmg", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("000,000").x);
        ImGui::TableSetupColumn("dps", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("000.0").x);
        ImGui::TableSetupColumn("pct", ImGuiTableColumnFlags_WidthFixed, ImGui::CalcTextSize("100.0%").x);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        Disabled("Name");
        ImGui::TableNextColumn();
        RightAligned("Dmg", true);
        ImGui::TableNextColumn();
        RightAligned("DPS", true);
        ImGui::TableNextColumn();
        RightAligned("%", true);

        for (const auto& member : view.members) {
            ImGui::PushStyleColor(ImGuiCol_Text, kSlotColors[std::clamp(member.slot, 0, kPartySlots - 1)]);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            Text((member.isLocal ? "*" : "") + member.name);
            ImGui::TableNextColumn();
            RightAligned(ThousandsSep(member.damage));
            ImGui::TableNextColumn();
            RightAligned(StrFormat("%.1f", member.dps));
            ImGui::TableNextColumn();
            RightAligned(StrFormat("%.1f%%", member.percent));
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }

    ImGui::Separator();
    Text(StrFormat("Total  %s   %.1f DPS", ThousandsSep(view.total).c_str(), static_cast<float>(view.total) / duration));
    if (view.showCarts) {
        ImGui::SameLine();
        Disabled(StrFormat("  Carts %d/%d", view.deaths, view.maxDeaths));
    }
}

void DrawTrial(const meter::TrialView& trial, const Settings& s) {
    ImGui::Separator();
    switch (trial.state) {
        case meter::TrialState::Armed:
            Colored(kTrialRunning, StrFormat("Time trial %ds armed", trial.duration));
            Disabled(StrFormat("Clock starts on your first hit.  %s cancels", s.keyTrial.text.c_str()));
            break;

        case meter::TrialState::Running: {
            const ImVec4 color = trial.remaining <= kTrialWarningSeconds ? kTrialEnding : kTrialRunning;
            Colored(color, StrFormat("Time trial %ds   %.1fs left", trial.duration, trial.remaining));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, color);
            ImGui::ProgressBar(trial.duration > 0 ? trial.elapsed / static_cast<float>(trial.duration) : 0.0f,
                               ImVec2(-1.0f, std::max(4.0f, ImGui::GetFontSize() * 0.4f)), "");
            ImGui::PopStyleColor();
            Text(StrFormat("%s dmg   %.1f DPS   %d hits   %d crit", ThousandsSep(trial.damage).c_str(), trial.dps, trial.hits,
                           trial.crits));
            if (trial.previousBest)
                Disabled(StrFormat("Best %s (%.1f DPS)", ThousandsSep(*trial.previousBest).c_str(),
                                   static_cast<float>(*trial.previousBest) / static_cast<float>(trial.duration)));
            break;
        }

        case meter::TrialState::Finished: {
            Colored(kTrialRunning, StrFormat("Time trial %ds finished", trial.duration));
            if (trial.personalBest) {
                ImGui::SameLine();
                Colored(kTrialBest, trial.previousBest
                                        ? StrFormat("NEW BEST (+%s)", ThousandsSep(trial.damage - *trial.previousBest).c_str())
                                        : std::string("FIRST RECORD"));
            } else if (trial.previousBest) {
                ImGui::SameLine();
                const int delta = trial.damage - *trial.previousBest;
                Disabled(StrFormat("best %s (%s%s)", ThousandsSep(*trial.previousBest).c_str(), delta >= 0 ? "+" : "",
                                   ThousandsSep(delta).c_str()));
            }
            Text(StrFormat("%s dmg   %.1f DPS   %d hits   %d crit", ThousandsSep(trial.damage).c_str(), trial.dps, trial.hits,
                           trial.crits));
            for (const auto& move : trial.topMoves) {
                const float share = trial.damage > 0 ? 100.0f * static_cast<float>(move.damage) / static_cast<float>(trial.damage) : 0.0f;
                Disabled(StrFormat("  %-22s %7s  %3.0f%%  x%d", meter::TimeTrial::ShortMoveName(move.name).c_str(),
                                   ThousandsSep(move.damage).c_str(), share, move.hits));
            }
            Disabled(StrFormat("%s runs it again   %s clears", s.keyTrial.text.c_str(), s.keyReset.text.c_str()));
            break;
        }

        default:
            break;
    }
}

void DrawOverlay(const meter::OverlayView& view, const Settings& s, bool menuOpen) {
    if (!s.overlayVisible || !view.show)
        return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!menuOpen)
        flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus;

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16.0f, 16.0f), ImGuiCond_FirstUseEver, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(std::clamp(s.overlayOpacity, 0.2f, 1.0f) * 0.85f);
    if (!ImGui::Begin("DPS Meter##MhwDpsMeterNative", nullptr, flags)) {
        ImGui::End();
        return;
    }

    Text("DPS");
    ImGui::SameLine();
    Disabled(view.training ? "training" : "");
    ImGui::Separator();

    if (view.members.empty()) {
        Disabled("Waiting for hunt data...");
        ImGui::End();
        return;
    }

    DrawPartyTable(view);
    if (view.trialActive)
        DrawTrial(view.trial, s);
    else if (!view.hint.empty())
        Disabled(view.hint);
    ImGui::End();
}

ImVec4 HealthColor(float fraction) {
    // Muted enough that the white HP numbers on top stay readable.
    if (fraction > 0.5f)
        return {0.24f, 0.62f, 0.30f, 1.0f};
    if (fraction > 0.25f)
        return {0.80f, 0.58f, 0.14f, 1.0f};
    return {0.78f, 0.24f, 0.20f, 1.0f};
}

/// Large-monster health bars, top centre by default (movable while F9 is open).
void DrawMonsters(const meter::OverlayView& view, const Settings& s, bool menuOpen) {
    if (!s.overlayVisible || view.monsters.empty())
        return;

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing;
    if (!menuOpen)
        flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus;

    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 16.0f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowBgAlpha(std::clamp(s.overlayOpacity, 0.2f, 1.0f) * 0.85f);
    if (!ImGui::Begin("Monsters##MhwDpsMeterNativeMonsters", nullptr, flags)) {
        ImGui::End();
        return;
    }

    const float font = ImGui::GetFontSize();
    const float width = font * 18.0f;
    for (size_t i = 0; i < view.monsters.size(); ++i) {
        const auto& monster = view.monsters[i];
        const float fraction = std::clamp(monster.health / monster.maxHealth, 0.0f, 1.0f);
        const std::string percent = StrFormat("%.0f%%", fraction * 100.0f);
        const float startX = ImGui::GetCursorPosX();
        Text(monster.name);
        ImGui::SameLine(startX + width - ImGui::CalcTextSize(percent.c_str()).x);
        Text(percent);

        const std::string label = ThousandsSep(std::lround(monster.health)) + " / " + ThousandsSep(std::lround(monster.maxHealth));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, HealthColor(fraction));
        ImGui::ProgressBar(fraction, ImVec2(width, std::round(font * 1.05f)), label.c_str());
        ImGui::PopStyleColor();
        if (i + 1 < view.monsters.size())
            ImGui::Spacing();
    }
    ImGui::End();
}

// ---- F9 panel ---------------------------------------------------------------------

int g_expandedHistory = -1;

void DrawTrialSettings(const meter::PanelView& panel, const Settings& s) {
    ImGui::Separator();
    Text("Time trial (training area)");

    int duration = s.trialDurationSeconds;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
    if (ImGui::InputInt("Duration (s)", &duration, 5, 30))
        settings::Update([&](Settings& value) { value.trialDurationSeconds = duration; });
    for (int preset : meter::TimeTrial::kPresets) {
        ImGui::SameLine();
        if (ImGui::SmallButton(StrFormat("%ds", preset).c_str()))
            settings::Update([&](Settings& value) { value.trialDurationSeconds = preset; });
    }

    if (!panel.training) {
        Disabled(StrFormat("Enter the training area to run a trial. %s arms it; the clock starts on your first hit.",
                           s.keyTrial.text.c_str()));
    } else {
        std::string label;
        switch (panel.trialState) {
            case meter::TrialState::Armed:
            case meter::TrialState::Running: label = StrFormat("Cancel trial (%s)", s.keyTrial.text.c_str()); break;
            case meter::TrialState::Finished:
                label = StrFormat("Run again: %ds (%s)", s.trialDurationSeconds, s.keyTrial.text.c_str());
                break;
            default: label = StrFormat("Start %ds trial (%s)", s.trialDurationSeconds, s.keyTrial.text.c_str()); break;
        }
        if (ImGui::Button(label.c_str()))
            meter::Instance().Post(meter::Command::ToggleTrial);
    }

    const auto bests = meter::Logs().Bests();
    if (bests.empty()) {
        Disabled("No trials recorded yet.");
        return;
    }
    Disabled("Personal bests");
    for (const auto& entry : bests) {
        Text(StrFormat("  %-14s %4.0fs  %9s dmg  %7.1f DPS   %s", entry.weapon.c_str(), entry.durationSeconds,
                       ThousandsSep(entry.totalDamage).c_str(),
                       static_cast<float>(entry.totalDamage) / std::max(entry.durationSeconds, 1.0f),
                       LocalTimeString(entry.startedAtMs, "%Y-%m-%d").c_str()));
    }
}

void DrawHistory() {
    ImGui::Separator();
    Text("Fight logs");
    const auto history = meter::Logs().History();
    if (history.empty()) {
        Disabled("No hunts recorded yet.");
        return;
    }
    Disabled(WideToUtf8(meter::Logs().Directory()));
    for (int i = 0; i < static_cast<int>(history.size()); ++i) {
        const auto& log = history[static_cast<size_t>(i)];
        const std::string top = log.players.empty() ? "-" : log.players.front().name;
        const std::string label =
            StrFormat("%s  %s  %.0fs  %s##hist%d", log.questName.c_str(), log.result.c_str(), log.durationSeconds, top.c_str(), i);
        const bool open = g_expandedHistory == i;
        if (ImGui::Selectable(label.c_str(), open))
            g_expandedHistory = open ? -1 : i;
        if (!open)
            continue;
        ImGui::Indent();
        Disabled(log.fileName);
        if (!log.monsters.empty()) {
            std::string monsters;
            for (const auto& name : log.monsters)
                monsters += (monsters.empty() ? "" : ", ") + name;
            Disabled(monsters);
        }
        for (const auto& player : log.players) {
            Text(StrFormat("%-16s %9s  %7.1f  %5.1f%%  %s", player.name.c_str(), ThousandsSep(player.damage).c_str(), player.dps,
                           player.percent, player.weapon.c_str()));
        }
        if (log.hits > 0)
            Disabled(StrFormat("%zu hits, %zu events recorded", log.hits, log.events));
        ImGui::Unindent();
    }
}

void DrawPanel(const Settings& s) {
    const meter::PanelView panel = meter::Instance().Panel();
    const float font = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(font * 46.0f, font * 40.0f), ImGuiCond_FirstUseEver);
    // Below the top-centre monster bars so both stay visible.
    ImGui::SetNextWindowPos(ImVec2(font * 2.0f, font * 12.0f), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("MHW DPS Meter (native)##MhwDpsMeterNativePanel", &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        if (!open)
            input::SetMenuOpen(false);
        return;
    }

    Text(meter::Instance().Status());
    Disabled(StrFormat("native %s", kBuildStamp));

    if (ImGui::Button(StrFormat("Dump diagnostics (%s)", s.keyDiagnostics.text.c_str()).c_str()))
        meter::Instance().Post(meter::Command::DumpDiagnostics);
    if (panel.training) {
        ImGui::SameLine();
        if (ImGui::Button(StrFormat("Reset training damage (%s)", s.keyReset.text.c_str()).c_str()))
            meter::Instance().Post(meter::Command::ResetTraining);
    }

    bool visible = s.overlayVisible;
    if (ImGui::Checkbox(StrFormat("Show overlay (%s)", s.keyOverlay.text.c_str()).c_str(), &visible))
        settings::Update([&](Settings& value) { value.overlayVisible = visible; });
    ImGui::SameLine();
    bool monsterBars = s.showMonsterHealth;
    if (ImGui::Checkbox("Monster health bars", &monsterBars))
        settings::Update([&](Settings& value) { value.showMonsterHealth = monsterBars; });

    float opacity = s.overlayOpacity;
    ImGui::SetNextItemWidth(font * 12.0f);
    if (ImGui::SliderFloat("Opacity", &opacity, 0.25f, 1.0f, "%.2f"))
        settings::Update([&](Settings& value) { value.overlayOpacity = opacity; });

    bool autoSize = s.fontSize <= 0.0f;
    if (ImGui::Checkbox("Automatic text size", &autoSize))
        settings::Update([&](Settings& value) { value.fontSize = autoSize ? 0.0f : std::max(14.0f, std::round(font)); });
    if (!autoSize) {
        float size = s.fontSize;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(font * 10.0f);
        if (ImGui::SliderFloat("Text size", &size, 10.0f, 40.0f, "%.0f px"))
            settings::Update([&](Settings& value) { value.fontSize = std::round(size); });
    }

    Disabled("While this panel is open, drag the DPS Meter and Monsters windows by their title bars.");
    Text(StrFormat("%s overlay   %s panel   %s diagnostics   %s reset training   %s time trial", s.keyOverlay.text.c_str(),
                   s.keyMenu.text.c_str(), s.keyDiagnostics.text.c_str(), s.keyReset.text.c_str(), s.keyTrial.text.c_str()));
    Disabled("Damage shows after you depart on a quest and in the training area; monster health on any field map.");

    DrawTrialSettings(panel, s);
    DrawHistory();

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Diagnostics")) {
        Wrapped(panel.diagnostics);
        if (ImGui::TreeNode("Recent log lines")) {
            for (const auto& line : log::Recent(40))
                Wrapped(line);
            ImGui::TreePop();
        }
    }
    ImGui::End();
    if (!open)
        input::SetMenuOpen(false);
}

}  // namespace

void ApplyStyle() {
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 3.0f;
    style.GrabRounding = 3.0f;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.CellPadding = ImVec2(6.0f, 2.0f);
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.07f, 0.09f, 1.0f);
}

void Draw(bool menuOpen) {
    const Settings s = settings::Get();
    const meter::OverlayView view = meter::Instance().Overlay();
    DrawMonsters(view, s, menuOpen);
    DrawOverlay(view, s, menuOpen);
    if (menuOpen)
        DrawPanel(s);
}

}  // namespace dps::render::overlay
