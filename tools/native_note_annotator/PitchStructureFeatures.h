#pragma once

#include <array>
#include <atomic>
#include <vector>

namespace native_annotation
{
struct NoteAnalysis;
inline constexpr int kPitchStructureFeatureCount = 14;

// Offline worker only. No note decisions or MIDI classes are produced here.
std::vector<std::array<float, kPitchStructureFeatureCount>> makePitchStructureFeatures(
    const NoteAnalysis& analysis, int frames, const std::atomic<bool>& cancelled);
}
