#pragma once

namespace dps::render::overlay {

void ApplyStyle();
/// Between ImGui::NewFrame and ImGui::Render: the DPS overlay and, when open, the F9 panel.
void Draw(bool menuOpen);

}  // namespace dps::render::overlay
