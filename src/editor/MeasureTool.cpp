#include "MeasureTool.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "../graphics/wicked/imgui/imgui.h"

void MeasureTool::render(ImDrawList* drawList, ToPixelFn toPixel, void* userData, float speed) {
    if (state != PLACING_B && state != COMPLETE)
        return;

    ImVec2 pA = toPixel(latA, lonA, userData);
    ImVec2 pB = toPixel(latB, lonB, userData);

    // Line colour: yellow while placing, green when complete
    ImU32 lineCol = (state == COMPLETE) ? IM_COL32(50, 255, 50, 220)
                                         : IM_COL32(255, 255, 50, 200);

    // Draw line
    drawList->AddLine(pA, pB, lineCol, 2.0f);

    // Endpoint markers
    drawList->AddCircleFilled(pA, 4.0f, lineCol);
    drawList->AddCircleFilled(pB, 4.0f, lineCol);

    // Labels "A" and "B"
    drawList->AddText(ImVec2(pA.x - 10, pA.y - 16), IM_COL32(255, 255, 255, 220), "A");
    drawList->AddText(ImVec2(pB.x + 6, pB.y - 16), IM_COL32(255, 255, 255, 220), "B");

    // Compute measurement values
    double distNM = getDistNM();
    double distKm = distNM * 1.852;
    double brg = getBearing();

    // Transit time
    double transitMin = (speed > 0) ? (distNM / speed) * 60.0 : 0;

    // Build label text
    char label1[64], label2[64];
    snprintf(label1, sizeof(label1), "%.1f NM (%.1f km)  %03.0f%cT",
             distNM, distKm, brg, 0xB0); // degree symbol
    if (speed > 0 && transitMin > 0) {
        if (transitMin < 60)
            snprintf(label2, sizeof(label2), "%.0f min @ %.0f kt", transitMin, speed);
        else
            snprintf(label2, sizeof(label2), "%.1f hr @ %.0f kt", transitMin / 60.0, speed);
    } else {
        label2[0] = '\0';
    }

    // Position label at midpoint
    ImVec2 mid = ImVec2((pA.x + pB.x) * 0.5f, (pA.y + pB.y) * 0.5f);

    // Background for readability
    ImVec2 t1Size = ImGui::CalcTextSize(label1);
    ImVec2 t2Size = ImGui::CalcTextSize(label2);
    float bgW = (t1Size.x > t2Size.x) ? t1Size.x : t2Size.x;
    float bgH = t1Size.y + (label2[0] ? t2Size.y + 2 : 0);

    float labelX = mid.x - bgW * 0.5f;
    float labelY = mid.y - bgH - 8;

    drawList->AddRectFilled(ImVec2(labelX - 4, labelY - 2),
                            ImVec2(labelX + bgW + 4, labelY + bgH + 4),
                            IM_COL32(0, 0, 0, 180), 3.0f);

    drawList->AddText(ImVec2(labelX, labelY), IM_COL32(255, 255, 255, 240), label1);
    if (label2[0]) {
        drawList->AddText(ImVec2(labelX, labelY + t1Size.y + 2),
                          IM_COL32(200, 200, 200, 200), label2);
    }
}
