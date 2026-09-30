#pragma once

namespace SellCompletion
{
    using LogCallback = void (*)(const char* message);

    void Initialize(LogCallback logger);
    void ConfigureCooldown(
        bool enabled,
        int minutes);
    bool TryGetCooldownClockOverride(
        int& clockValue,
        int& remainingSeconds);
    void Update(
        bool shopActive,
        bool sellContextActive,
        int sellPrice,
        int sellControlState);
    void Reset();
}