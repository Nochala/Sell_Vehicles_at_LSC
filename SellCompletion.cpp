#include <windows.h>
#include <cstdio>
#include <cstdarg>

#include "script.h"
#include "natives.h"
#include "SellCompletion.h"

namespace
{
    enum class CompletionState
    {
        Idle,
        WaitingForFade,
        FadingOut,
        DetachingPed,
        HoldingBlack,
        FadingIn
    };

    struct ExteriorPoint
    {
        float x;
        float y;
        float z;
        float heading;
        const char* name;
    };

    static const ExteriorPoint kExteriorPoints[] =
    {
        { -362.7962f, -132.4005f, 38.25239f, 71.187133f, "Burton" },
        { -1140.191f, -1985.478f, 12.72923f, 315.290466f, "LSIA" },
        { 716.4645f, -1088.869f, 21.92979f, 88.768f, "La Mesa" },
        { 1174.811f, 2649.954f, 37.37151f, 0.450f, "Harmony" },
        { 118.6830f, 6618.4130f, 30.9185f, 0.450f, "Paleto Bay" }
    };

    static constexpr ULONGLONG kPostConfirmDelayMs = 500ULL;
    static constexpr int kFadeOutMs = 350;
    static constexpr int kFadeInMs = 500;
    static constexpr ULONGLONG kFadeOutTimeoutMs = 1500ULL;
    static constexpr ULONGLONG kDetachTimeoutMs = 250ULL;
    static constexpr ULONGLONG kBlackHoldMs = 150ULL;

    static SellCompletion::LogCallback g_logger = nullptr;
    static CompletionState g_state = CompletionState::Idle;

    static bool g_sellContextActive = false;
    static bool g_confirmationSeen = false;
    static int g_lastSellPrice = 0;

    static Vehicle g_soldVehicle = 0;
    static Ped g_playerPed = 0;
    static ExteriorPoint g_exitPoint{};
    static ULONGLONG g_stateStartedAt = 0;
    static ULONGLONG g_actionAt = 0;

    static void Log(const char* format, ...)
    {
        if (!g_logger || !format)
            return;

        char buffer[512]{};

        va_list args;
        va_start(args, format);
        vsnprintf_s(
            buffer,
            sizeof(buffer),
            _TRUNCATE,
            format,
            args);
        va_end(args);

        g_logger(buffer);
    }

    static float DistanceSquared(
        const Vector3& left,
        const ExteriorPoint& right)
    {
        const float dx = left.x - right.x;
        const float dy = left.y - right.y;
        const float dz = left.z - right.z;

        return dx * dx + dy * dy + dz * dz;
    }

    static ExteriorPoint FindNearestExterior(
        const Vector3& position)
    {
        size_t bestIndex = 0;
        float bestDistance =
            DistanceSquared(
                position,
                kExteriorPoints[0]);

        for (size_t i = 1;
             i < sizeof(kExteriorPoints)
                 / sizeof(kExteriorPoints[0]);
             ++i)
        {
            const float distance =
                DistanceSquared(
                    position,
                    kExteriorPoints[i]);

            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = i;
            }
        }

        return kExteriorPoints[bestIndex];
    }

    static void ClearSellContext()
    {
        g_sellContextActive = false;
        g_confirmationSeen = false;
        g_lastSellPrice = 0;
    }

    static void ResetCompletionState()
    {
        g_state = CompletionState::Idle;
        g_soldVehicle = 0;
        g_playerPed = 0;
        g_exitPoint = ExteriorPoint{};
        g_stateStartedAt = 0;
        g_actionAt = 0;
    }

    static bool CaptureSaleTarget()
    {
        const Ped ped =
            PLAYER::PLAYER_PED_ID();

        if (!PED::IS_PED_IN_ANY_VEHICLE(
                ped,
                false))
        {
            Log(
                "completion not armed: player is not in a vehicle");
            return false;
        }

        const Vehicle vehicle =
            PED::GET_VEHICLE_PED_IS_IN(
                ped,
                false);

        if (vehicle == 0
            || !ENTITY::DOES_ENTITY_EXIST(vehicle))
        {
            Log(
                "completion not armed: vehicle handle is invalid");
            return false;
        }

        const Vector3 position =
            ENTITY::GET_ENTITY_COORDS(
                vehicle,
                true);

        g_playerPed = ped;
        g_soldVehicle = vehicle;
        g_exitPoint =
            FindNearestExterior(position);

        Log(
            "armed vehicle=%d price=%d exit=%s pos=(%.3f, %.3f, %.3f)",
            static_cast<int>(g_soldVehicle),
            g_lastSellPrice,
            g_exitPoint.name,
            position.x,
            position.y,
            position.z);

        return true;
    }

    static void DeleteSoldVehicle()
    {
        if (g_soldVehicle == 0
            || !ENTITY::DOES_ENTITY_EXIST(
                g_soldVehicle))
        {
            return;
        }

        ENTITY::SET_ENTITY_AS_MISSION_ENTITY(
            g_soldVehicle,
            true,
            true);

        Vehicle vehicle = g_soldVehicle;
        VEHICLE::DELETE_VEHICLE(&vehicle);

        if (ENTITY::DOES_ENTITY_EXIST(
                g_soldVehicle))
        {
            Entity entity =
                static_cast<Entity>(
                    g_soldVehicle);
            ENTITY::DELETE_ENTITY(&entity);
        }

        Log(
            "vehicle delete requested handle=%d remaining=%s",
            static_cast<int>(g_soldVehicle),
            ENTITY::DOES_ENTITY_EXIST(
                g_soldVehicle)
                ? "yes"
                : "no");
    }

    static void TeleportPlayerOutside()
    {
        if (g_playerPed == 0
            || !ENTITY::DOES_ENTITY_EXIST(
                g_playerPed))
        {
            g_playerPed =
                PLAYER::PLAYER_PED_ID();
        }

        if (g_playerPed == 0
            || !ENTITY::DOES_ENTITY_EXIST(
                g_playerPed))
        {
            Log(
                "teleport skipped: player ped unavailable");
            return;
        }

        ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
            g_playerPed,
            g_exitPoint.x,
            g_exitPoint.y,
            g_exitPoint.z,
            false,
            false,
            false);

        ENTITY::SET_ENTITY_HEADING(
            g_playerPed,
            g_exitPoint.heading);

        Log(
            "teleported player exit=%s pos=(%.3f, %.3f, %.3f) heading=%.3f",
            g_exitPoint.name,
            g_exitPoint.x,
            g_exitPoint.y,
            g_exitPoint.z,
            g_exitPoint.heading);
    }

    static bool ArmPostSaleTransition(
        ULONGLONG now,
        const char* source,
        int sellControlState)
    {
        if (!CaptureSaleTarget())
            return false;

        g_state =
            CompletionState::WaitingForFade;
        g_stateStartedAt = now;
        g_actionAt =
            now + kPostConfirmDelayMs;

        Log(
            "post-sale transition scheduled source=%s iControl=%d delayMs=%llu",
            source ? source : "unknown",
            sellControlState,
            static_cast<unsigned long long>(
                kPostConfirmDelayMs));

        return true;
    }

    static void UpdateCompletionState(
        ULONGLONG now)
    {
        switch (g_state)
        {
        case CompletionState::Idle:
            return;

        case CompletionState::WaitingForFade:
            if (now < g_actionAt)
                return;

            CAM::DO_SCREEN_FADE_OUT(
                kFadeOutMs);
            g_state =
                CompletionState::FadingOut;
            g_stateStartedAt = now;

            Log("fade out started");
            return;

        case CompletionState::FadingOut:
            if (!CAM::IS_SCREEN_FADED_OUT()
                && now - g_stateStartedAt
                    < kFadeOutTimeoutMs)
            {
                return;
            }

            if (g_playerPed != 0
                && g_soldVehicle != 0
                && ENTITY::DOES_ENTITY_EXIST(
                    g_soldVehicle)
                && PED::IS_PED_IN_VEHICLE(
                    g_playerPed,
                    g_soldVehicle,
                    false))
            {
                AI::TASK_LEAVE_VEHICLE(
                    g_playerPed,
                    g_soldVehicle,
                    16);
            }

            g_state =
                CompletionState::DetachingPed;
            g_stateStartedAt = now;
            return;

        case CompletionState::DetachingPed:
            if (g_playerPed != 0
                && g_soldVehicle != 0
                && ENTITY::DOES_ENTITY_EXIST(
                    g_soldVehicle)
                && PED::IS_PED_IN_VEHICLE(
                    g_playerPed,
                    g_soldVehicle,
                    false)
                && now - g_stateStartedAt
                    < kDetachTimeoutMs)
            {
                return;
            }

            DeleteSoldVehicle();
            TeleportPlayerOutside();

            g_state =
                CompletionState::HoldingBlack;
            g_stateStartedAt = now;
            return;

        case CompletionState::HoldingBlack:
            if (now - g_stateStartedAt
                < kBlackHoldMs)
            {
                return;
            }

            CAM::DO_SCREEN_FADE_IN(
                kFadeInMs);
            g_state =
                CompletionState::FadingIn;
            g_stateStartedAt = now;

            Log("fade in started");
            return;

        case CompletionState::FadingIn:
            if (!CAM::IS_SCREEN_FADED_IN()
                && now - g_stateStartedAt
                    < static_cast<ULONGLONG>(
                        kFadeInMs + 1000))
            {
                return;
            }

            Log("completion finished");
            ResetCompletionState();
            ClearSellContext();
            return;
        }
    }
}

namespace SellCompletion
{
    void Initialize(LogCallback logger)
    {
        g_logger = logger;
        Reset();
    }

    void Reset()
    {
        ClearSellContext();
        ResetCompletionState();
    }

    void Update(
        bool shopActive,
        bool sellContextActive,
        int sellPrice,
        int sellControlState)
    {
        const ULONGLONG now =
            GetTickCount64();

        UpdateCompletionState(now);

        if (g_state != CompletionState::Idle)
            return;

        if (!shopActive)
        {
            ClearSellContext();
            return;
        }

        if (sellPrice > 0)
            g_lastSellPrice = sellPrice;

        // No input counting or timing heuristics are used here. Completion is
        // driven only by Rockstar's structurally resolved DO_STAGE_SELL state.
        if (sellControlState < 0)
        {
            ClearSellContext();
            return;
        }

        if (sellControlState == 0)
        {
            g_sellContextActive =
                sellContextActive
                && sellPrice > 0;
            g_confirmationSeen = false;
            return;
        }

        if (sellControlState == 1)
        {
            if (!g_sellContextActive
                && !(sellContextActive
                    && sellPrice > 0))
            {
                return;
            }

            g_sellContextActive = true;

            if (!g_confirmationSeen)
            {
                g_confirmationSeen = true;
                Log(
                    "Rockstar final Sell confirmation displayed iControl=1 price=%d",
                    g_lastSellPrice);
            }

            return;
        }

        if (sellControlState == 2
            || sellControlState == 3)
        {
            if (!g_sellContextActive
                || !g_confirmationSeen)
            {
                return;
            }

            if (!ArmPostSaleTransition(
                    now,
                    "rockstar-confirmed",
                    sellControlState))
            {
                ClearSellContext();
            }

            return;
        }

        ClearSellContext();
    }
}
