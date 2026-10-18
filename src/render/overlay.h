#pragma once

namespace dps::render::overlay {

void ApplyStyle();
/// Between ImGui::NewFrame and ImGui::Render: the DPS overlay, monster bars and, when
/// open, the F9 panel. fontSize is the overlay's base text size in pixels.
void Draw(bool menuOpen, float fontSize);

}  // namespace dps::render::overlay
