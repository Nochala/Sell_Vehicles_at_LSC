#pragma once

namespace SellCompletion
{
    using LogCallback = void (*)(const char* message);

    void Initialize(LogCallback logger);
    void Update(
        bool shopActive,
        bool sellContextActive,
        int sellPrice,
        bool sellConfirmationActive,
        bool acceptPressed,
        bool cancelPressed);
    void Reset();
}
