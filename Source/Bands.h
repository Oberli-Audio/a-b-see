#pragma once

#include <array>

/*  A B see - the seven bands of the band deltas (sketch 4.5), as plain data: what a section of
    a reference carries (ReferenceStore.h) and what BandDeltas.h measures and compares. */
namespace absee
{
inline constexpr int kNumBands = 7;
inline constexpr float kNoLevel = -120.0f;     ///< oberli::Spectrum's "no energy"

using BandLevels = std::array<float, kNumBands>;

/** The upper edge of each band in Hz; the last band is open above. */
inline constexpr std::array<float, kNumBands - 1> kBandUpperEdgesHz { 60.0f, 150.0f, 400.0f, 1500.0f, 4000.0f, 10000.0f };
inline constexpr std::array<const char*, kNumBands> kBandNames { "Sub", "Low", "Lo-mid", "Mid", "Hi-mid", "High", "Air" };

inline BandLevels noBandLevels() noexcept
{
    BandLevels b;
    b.fill (kNoLevel);
    return b;
}
} // namespace absee
