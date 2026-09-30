#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <share.h>

#include "script.h"
#include "natives.h"
#include "SellCompletion.h"

static const char* g_iniPath = ".\\SellVehiclesAtLSC.ini";
static const char* g_logPath = "SellVehiclesAtLSC.log";
static const char* kBuildTag = "v0.3.22 LSC-only clean logging";

static bool g_enabled = true;
static bool g_useSellCooldown = false;
static int g_sellCooldownMinutes = 48;
static int g_vehicleSellPercent = 60;
static int g_upgradePercent = 8;
static bool g_useDamagePenalty = true;
static bool g_allowCharacterVehicles = false;

static bool g_logEnabled = true;
static bool g_showStartupNotification = false;
static bool g_logControls = false;
static bool g_logVehicleSnapshots = false;
static int g_scriptPollIntervalMs = 250;
static int g_snapshotIntervalMs = 1000;

static constexpr size_t kLogBufferCapacity = 128 * 1024;
static constexpr ULONGLONG kLogFlushIntervalMs = 1000ULL;
static char g_logBuffer[kLogBufferCapacity]{};
static size_t g_logBufferUsed = 0;
static uint32_t g_logDroppedLines = 0;
static ULONGLONG g_nextLogFlushAt = 0;
static bool g_debugLogActive = false;

static bool g_phase2Enabled = true;
static bool g_phase3Enabled = true;

static constexpr uint64_t kDoesScriptWithNameHashExistNative =
    0xF86AA3C56BA31381ULL;
static constexpr uint64_t kHasScriptWithNameHashLoadedNative =
    0x5F0F0C783EB16C04ULL;
static constexpr uint64_t kGetNumberOfThreadsRunningScriptHashNative =
    0x2C83A9DA6BFFC4F9ULL;
static constexpr uint64_t kGetVehicleModelValueNative =
    0x5873C14A52D74236ULL;

struct ScriptProbe
{
    const char* name;
    uint32_t hash;
    bool exists;
    bool loaded;
    int runningCount;
    bool initialized;
};

static ScriptProbe g_scriptProbes[] =
{
    { "carmod_shop", 0, false, false, 0, false },
};

static constexpr size_t kScriptProbeCount =
    sizeof(g_scriptProbes) / sizeof(g_scriptProbes[0]);

static bool g_carmodShopActive = false;
static bool g_lastNetworkGame = false;
static bool g_networkStateInitialized = false;
static uint32_t g_shopSessionId = 0;
static ULONGLONG g_shopSessionStartedAt = 0;
static ULONGLONG g_nextScriptPollAt = 0;
static ULONGLONG g_nextSnapshotAt = 0;
static ULONGLONG g_nextPhase2ProgramCheckAt = 0;
static SHORT g_lastF10State = 0;
static bool g_rootMarkerSet = false;
static int g_inferredMenuDepth = -1;
static uint32_t g_inputSequence = 0;
static bool g_stockLscScopeActive = false;

struct StockLscPoint
{
    float x;
    float y;
    float z;
    const char* name;
};

static const StockLscPoint kStockLscPoints[] =
{
    { -362.7962f, -132.4005f, 38.25239f, "Burton" },
    { -1140.191f, -1985.478f, 12.72923f, "LSIA" },
    { 716.4645f, -1088.869f, 21.92979f, "La Mesa" },
    { 1174.811f, 2649.954f, 37.37151f, "Harmony" }
};

static constexpr float kStockLscPatchRadius = 140.0f;

struct VehicleSnapshot
{
    Vehicle vehicle;
    Hash model;
    int vehicleClass;
    int modKit;
    int entityHealth;
    float engineHealth;
    float bodyHealth;
    Vector3 coords;
    bool valid;
};

static VehicleSnapshot g_lastVehicleSnapshot{};

static int ReadIniInt(
    const char* section,
    const char* key,
    int defaultValue)
{
    return GetPrivateProfileIntA(
        section,
        key,
        defaultValue,
        g_iniPath);
}

static bool ReadIniBool(
    const char* section,
    const char* key,
    bool defaultValue)
{
    char value[32]{};

    GetPrivateProfileStringA(
        section,
        key,
        defaultValue ? "true" : "false",
        value,
        static_cast<DWORD>(sizeof(value)),
        g_iniPath);

    if (_stricmp(value, "true") == 0
        || _stricmp(value, "yes") == 0
        || _stricmp(value, "on") == 0
        || std::strcmp(value, "1") == 0)
    {
        return true;
    }

    if (_stricmp(value, "false") == 0
        || _stricmp(value, "no") == 0
        || _stricmp(value, "off") == 0
        || std::strcmp(value, "0") == 0)
    {
        return false;
    }

    return defaultValue;
}

static void ResetLogFile()
{
    g_logBufferUsed = 0;
    g_logDroppedLines = 0;
    g_debugLogActive = false;
    g_nextLogFlushAt =
        GetTickCount64() + kLogFlushIntervalMs;

    if (!g_logEnabled)
        return;

    FILE* file = _fsopen(g_logPath, "w", _SH_DENYNO);
    if (file)
        fclose(file);
}

static void WriteStatusLogLine(
    const char* message)
{
    if (!g_logEnabled || !message)
        return;

    FILE* file =
        _fsopen(g_logPath, "a", _SH_DENYNO);

    if (!file)
        return;

    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);

    std::fprintf(
        file,
        "[%02u:%02u:%02u.%03u] %s\n",
        static_cast<unsigned int>(localTime.wHour),
        static_cast<unsigned int>(localTime.wMinute),
        static_cast<unsigned int>(localTime.wSecond),
        static_cast<unsigned int>(localTime.wMilliseconds),
        message);

    std::fclose(file);
}

static bool ContainsInsensitive(
    const char* text,
    const char* needle)
{
    if (!text || !needle || !*needle)
        return false;

    const size_t needleLength =
        std::strlen(needle);

    for (const char* p = text; *p; ++p)
    {
        if (_strnicmp(
                p,
                needle,
                needleLength) == 0)
        {
            return true;
        }
    }

    return false;
}

static bool IsFailureDiagnostic(
    const char* message)
{
    if (!message)
        return false;

    static const char* const failureMarkers[] =
    {
        "failed",
        "failure",
        "error=",
        "error:",
        "=no reason=",
        "unresolved",
        "could not",
        "not armed",
        "unsupported executable",
        "invalid code",
        "invalid program",
        "write/verify"
    };

    for (size_t i = 0;
         i < sizeof(failureMarkers)
             / sizeof(failureMarkers[0]);
         ++i)
    {
        if (ContainsInsensitive(
                message,
                failureMarkers[i]))
        {
            return true;
        }
    }

    return false;
}

static void FlushLogBuffer()
{
    if (!g_logEnabled
        || !g_debugLogActive
        || (g_logBufferUsed == 0
            && g_logDroppedLines == 0))
    {
        return;
    }

    FILE* file =
        _fsopen(g_logPath, "a", _SH_DENYNO);

    if (!file)
        return;

    if (g_logBufferUsed > 0)
    {
        std::fwrite(
            g_logBuffer,
            1,
            g_logBufferUsed,
            file);
    }

    if (g_logDroppedLines > 0)
    {
        std::fprintf(
            file,
            "[logger] dropped %u buffered debug log lines\n",
            static_cast<unsigned int>(
                g_logDroppedLines));
    }

    std::fclose(file);

    g_logBufferUsed = 0;
    g_logDroppedLines = 0;
}

static void Logf(const char* format, ...)
{
    if (!g_logEnabled || !format)
        return;

    char message[1792]{};

    va_list args;
    va_start(args, format);
    vsnprintf_s(
        message,
        sizeof(message),
        _TRUNCATE,
        format,
        args);
    va_end(args);

    char line[2048]{};

    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);

    _snprintf_s(
        line,
        sizeof(line),
        _TRUNCATE,
        "[%02u:%02u:%02u.%03u] [Debug] %s",
        static_cast<unsigned int>(localTime.wHour),
        static_cast<unsigned int>(localTime.wMinute),
        static_cast<unsigned int>(localTime.wSecond),
        static_cast<unsigned int>(localTime.wMilliseconds),
        message);

    size_t lineLength =
        strnlen_s(line, sizeof(line));

    if (lineLength < sizeof(line) - 1)
    {
        line[lineLength++] = '\n';
        line[lineLength] = '\0';
    }

    if (lineLength == 0
        || lineLength > kLogBufferCapacity)
    {
        ++g_logDroppedLines;
        return;
    }

    // Before a failure, keep diagnostics in memory only. If the history fills,
    // discard the older successful trace and retain the newest context.
    if (!g_debugLogActive
        && g_logBufferUsed
            > kLogBufferCapacity - lineLength)
    {
        g_logBufferUsed = 0;
        ++g_logDroppedLines;
    }

    if (g_logBufferUsed
        > kLogBufferCapacity - lineLength)
    {
        ++g_logDroppedLines;
        return;
    }

    std::memcpy(
        g_logBuffer + g_logBufferUsed,
        line,
        lineLength);

    g_logBufferUsed += lineLength;

    if (!g_debugLogActive
        && IsFailureDiagnostic(message))
    {
        g_debugLogActive = true;
        WriteStatusLogLine(
            "SellVehiclesAtLSC failure detected - debug logging enabled");
        FlushLogBuffer();
    }
}

static void LogSellCompletionMessage(const char* message)
{
    Logf(
        "[SellCompletion] %s",
        message ? message : "<null>");
}

static void Notify(const std::string& text)
{
    UI::_SET_NOTIFICATION_TEXT_ENTRY("STRING");
    UI::_ADD_TEXT_COMPONENT_STRING(
        const_cast<char*>(text.c_str()));
    UI::_DRAW_NOTIFICATION(false, false);
}

static uint32_t Joaat(const char* text)
{
    uint32_t hash = 0;
    if (!text)
        return hash;

    while (*text)
    {
        unsigned char c =
            static_cast<unsigned char>(*text++);

        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<unsigned char>(
                c + ('a' - 'A'));
        }

        hash += c;
        hash += (hash << 10);
        hash ^= (hash >> 6);
    }

    hash += (hash << 3);
    hash ^= (hash >> 11);
    hash += (hash << 15);
    return hash;
}

static bool IsPlayerNearStockLsc()
{
    const Ped ped =
        PLAYER::PLAYER_PED_ID();

    if (ped == 0
        || !ENTITY::DOES_ENTITY_EXIST(ped))
    {
        return false;
    }

    Entity entity =
        static_cast<Entity>(ped);

    if (PED::IS_PED_IN_ANY_VEHICLE(
            ped,
            false))
    {
        const Vehicle vehicle =
            PED::GET_VEHICLE_PED_IS_IN(
                ped,
                false);

        if (vehicle != 0
            && ENTITY::DOES_ENTITY_EXIST(
                vehicle))
        {
            entity =
                static_cast<Entity>(
                    vehicle);
        }
    }

    const Vector3 position =
        ENTITY::GET_ENTITY_COORDS(
            entity,
            true);

    const float radiusSquared =
        kStockLscPatchRadius
        * kStockLscPatchRadius;

    for (size_t i = 0;
         i < sizeof(kStockLscPoints)
             / sizeof(kStockLscPoints[0]);
         ++i)
    {
        const float dx =
            position.x - kStockLscPoints[i].x;
        const float dy =
            position.y - kStockLscPoints[i].y;

        if (dx * dx + dy * dy
            <= radiusSquared)
        {
            return true;
        }
    }

    return false;
}

static void RestoreStockLscPatches(
    const char* reason);

static const char* GetExecutableName()
{
    static char executableName[MAX_PATH]{};

    char fullPath[MAX_PATH]{};
    const DWORD length = GetModuleFileNameA(
        GetModuleHandleA(nullptr),
        fullPath,
        MAX_PATH);

    if (length == 0 || length >= MAX_PATH)
        return "<unknown>";

    const char* slash = std::strrchr(fullPath, '\\');
    const char* name = slash ? slash + 1 : fullPath;

    strncpy_s(
        executableName,
        sizeof(executableName),
        name,
        _TRUNCATE);

    return executableName;
}

static const char* GetEditionName()
{
    const char* executableName = GetExecutableName();

    if (_stricmp(executableName, "GTA5_Enhanced.exe") == 0)
        return "Enhanced";

    if (_stricmp(executableName, "GTA5.exe") == 0)
        return "Legacy";

    return "Unknown";
}


struct Phase2ScrProgram
{
    char pad0[16];
    unsigned char** codeBlocks;
    char pad18[4];
    int codeSize;
    char pad20[4];
    int localCount;
    char pad28[4];
    int nativeCount;
    int64_t* localOffset;
    char pad38[8];
    int64_t* nativeOffset;
    char pad48[16];
    int nameHash;
    char pad5C[4];
    char* name;
    char** stringsOffset;
    int stringSize;
    char pad74[12];
};

static_assert(sizeof(Phase2ScrProgram) == 0x80, "Unexpected scrProgram layout.");

struct LegacyScriptTableItem
{
    Phase2ScrProgram* program;
    char padding[4];
    int hash;
};

struct LegacyScriptTable
{
    LegacyScriptTableItem* table;
    char padding[16];
    int count;
};

struct ScrThreadArrayRaw
{
    void** data;
    uint16_t count;
    uint16_t capacity;
};

struct Phase2ThreadInfo
{
    void* thread;
    void* stack;
    uint32_t threadId;
    uint32_t threadState;
    uint32_t programCounter;
    uint32_t framePointer;
    uint32_t stackPointer;
    uint32_t stackSize;
};

struct VmFunctionRange
{
    int index;
    uint32_t start;
    uint32_t end;
    uint8_t argCount;
    bool found;
};

static constexpr uint32_t kCarmodShopHash = 0x1DC6B680U;
static constexpr int kEnhancedProgramCount = 176;

static constexpr unsigned char kVmNop = 0x00;
static constexpr unsigned char kVmIeq = 0x08;
static constexpr unsigned char kVmIne = 0x09;
static constexpr unsigned char kVmPushConstU8 = 0x25;
static constexpr unsigned char kVmPushConstU32 = 0x28;
static constexpr unsigned char kVmDrop = 0x2B;
static constexpr unsigned char kVmNative = 0x2C;
static constexpr unsigned char kVmEnter = 0x2D;
static constexpr unsigned char kVmLeave = 0x2E;
static constexpr unsigned char kVmLoad = 0x2F;
static constexpr unsigned char kVmLocalU8 = 0x37;
static constexpr unsigned char kVmLocalU8Load = 0x38;
static constexpr unsigned char kVmPushConstS16 = 0x43;
static constexpr unsigned char kVmLocalU16 = 0x4C;
static constexpr unsigned char kVmLocalU16Load = 0x4D;
static constexpr unsigned char kVmJ = 0x55;
static constexpr unsigned char kVmJz = 0x56;
static constexpr unsigned char kVmIeqJz = 0x57;
static constexpr unsigned char kVmIneJz = 0x58;
static constexpr unsigned char kVmCall = 0x5D;
static constexpr unsigned char kVmLocalU24 = 0x5E;
static constexpr unsigned char kVmLocalU24Load = 0x5F;
static constexpr unsigned char kVmPushConstU24 = 0x64;
static constexpr unsigned char kVmSwitch = 0x65;
static constexpr unsigned char kVmString = 0x66;
static constexpr unsigned char kVmPushConst0 = 0x71;
static constexpr unsigned char kVmPushConst1 = 0x72;

static constexpr unsigned char kVmArrayU8 = 0x34;
static constexpr unsigned char kVmArrayU8Load = 0x35;
static constexpr unsigned char kVmStaticU16 = 0x4F;
static constexpr unsigned char kVmIoffsetS16 = 0x46;

static bool g_phase2InternalsInitialized = false;
static bool g_phase2ProgramResolverReady = false;
static LegacyScriptTable* g_legacyScriptTable = nullptr;
static Phase2ScrProgram** g_enhancedPrograms = nullptr;
static ScrThreadArrayRaw* g_threadArray = nullptr;

static Phase2ScrProgram* g_phase2ObservedProgram = nullptr;
static Phase2ScrProgram* g_phase2AttemptedProgram = nullptr;
static Phase2ScrProgram* g_phase2PatchedProgram = nullptr;
static uint32_t g_phase2PatchPosition = 0;
static bool g_phase2PatchApplied = false;
static bool g_phase2VisibilityBypassActive = false;
static unsigned char g_phase2OriginalCall[4]{};
static unsigned char g_phase2VisibilityPatch[4]{};
static std::string g_phase2Status = "not attempted";
static uint16_t g_phase2NetworkGameNativeIndex = 0xFFFF;

static Phase2ScrProgram* g_phase3AnalyzedProgram = nullptr;
static std::vector<VmFunctionRange> g_phase3FunctionCatalog;
static VmFunctionRange g_phase3SellHandler{};
static bool g_phase3SellHandlerResolved = false;
static ULONGLONG g_phase3TraceUntil = 0;
static uint32_t g_phase3LastTracePc = 0xFFFFFFFFU;
static uint32_t g_phase3LastTraceFp = 0xFFFFFFFFU;
static uint32_t g_phase3LastTraceSp = 0xFFFFFFFFU;
static uint32_t g_phase3LastTraceState = 0xFFFFFFFFU;

struct Phase3NativeCallContext
{
    void* returnValue;
    uint32_t argCount;
    uint32_t padding0C;
    void* args;
};

using Phase3NativeHandler = void (*)(Phase3NativeCallContext*);

struct Phase3NativeProbe
{
    uint16_t nativeIndex;
    uint32_t site;
    uint8_t expectedArgs;
    uint8_t expectedReturns;
    const char* role;
    Phase3NativeHandler original;
    Phase3NativeHandler detour;
    bool installed;
    uint32_t calls;
    uint32_t matchedCalls;
    uint32_t unmatchedInSellLogged;

    // Phase 3F: keep the synchronous native hooks cheap. Rockstar can execute
    // these sites every frame while the Sell UI is open, so only emit a MATCH
    // line when the observed state actually changes.
    bool hasLoggedState;
    uint32_t lastLoggedSite;
    uint32_t lastLoggedContextArgCount;
    uint64_t lastLoggedArg0;
    uint64_t lastLoggedArg1;
    uint64_t lastLoggedReturnRaw;
    uint64_t lastLoggedFrameLocal2;
    bool lastLoggedReturnReadable;
    bool lastLoggedFrameLocal2Readable;
    uint32_t suppressedDuplicateLogs;
};

static Phase2ScrProgram* g_phase3NativeProbeProgram = nullptr;
static uint32_t g_phase3LaterStateGateASite = 0;
static uint32_t g_phase3LaterStateGateBSite = 0;
static uint32_t g_phase3SecondaryStateTestSite = 0;
static uint32_t g_phase3BitUpdateSites[5]{};
static uint32_t g_phase3DelaySites[3]{};

struct Phase3HelperGateSite
{
    uint32_t callSite;
    uint32_t functionStart;
    uint32_t nativeSite;
    int functionIndex;

    // Phase 3I: decoded Online-side predicate shape. The helper first checks
    // NETWORK_IS_GAME_IN_PROGRESS, then tests one script static against 45 and
    // a second static (base + signed offset) against a helper-specific state.
    uint16_t staticBaseIndex;
    int16_t staticStateOffset;
    int expectedPrimaryState;
    int expectedSecondaryState;
    bool predicateShapeDecoded;
};

static Phase3HelperGateSite g_phase3HelperGateSites[8]{};
static size_t g_phase3HelperGateSiteCount = 0;
static Phase3NativeHandler g_phase3HelperNetworkOriginal = nullptr;
static bool g_phase3HelperNetworkInstalled = false;
static void* g_phase3HelperTraceThread = nullptr;
static ULONGLONG g_phase3HelperTraceUntil = 0;
static uint32_t g_phase3HelperTraceSequence = 0;

struct VmPatchBackup
{
    uint32_t position;
    unsigned char original[4];
};

static Phase2ScrProgram* g_highValueSellPatchedProgram = nullptr;
static uint32_t g_highValueSellPatchPosition = 0;
static bool g_highValueSellPatchApplied = false;
static unsigned char g_highValueSellOriginal[4]{};

static VmFunctionRange g_phase3SellEligibilityFunction{};
static uint32_t g_phase3NoSell1MessagePush = 0;

static Phase2ScrProgram* g_sellOwnershipPatchedProgram = nullptr;
static std::vector<uint32_t> g_sellOwnershipPatchPositions;
static std::vector<VmPatchBackup> g_sellOwnershipPatchBackups;
static bool g_sellOwnershipPatchApplied = false;
static VmFunctionRange g_phase3PlayerOwnedHelper{};

struct Phase3SellPricePath
{
    bool resolved;
    uint16_t staticBaseIndex;
    int16_t fieldOffset;
    uint8_t arrayStride;
    uint32_t element0StaticIndex;
    uint32_t itemCostPush;
    uint32_t priceLoadPosition;
    uint32_t priceAddressPosition;
    uint32_t initializerCall;
    VmFunctionRange initializerFunction;
    uint32_t registerNativeSite;
    uint16_t registerNativeIndex;
};

struct Phase3SellControlPath
{
    bool resolved;
    uint32_t staticIndex;
    uint32_t switchPosition;
};

struct Phase3SellStagePath
{
    bool resolved;
    uint32_t staticIndex;
    int sellMenuValue;
    uint32_t switchPosition;
    uint32_t sellCallPosition;
    int functionIndex;
};

static Phase3SellPricePath g_phase3SellPricePath{};

struct Phase3SellDisplayPriceHook
{
    Phase2ScrProgram* program;
    uint32_t nativeSite;
    uint16_t nativeIndex;
    Phase3NativeHandler original;
    bool installed;
};

static Phase3SellDisplayPriceHook g_phase3SellDisplayPriceHook{};

struct Phase3SellCooldownPath
{
    bool resolved;
    uint32_t messagePush;
    uint32_t clockNativeSite;
    uint16_t clockNativeIndex;
};

struct Phase3SellCooldownHook
{
    Phase2ScrProgram* program;
    uint32_t nativeSite;
    uint16_t nativeIndex;
    Phase3NativeHandler original;
    bool installed;
};

static Phase3SellCooldownPath g_phase3SellCooldownPath{};
static Phase3SellCooldownHook g_phase3SellCooldownHook{};
static bool g_phase3CooldownGateEventPending = false;
static int g_phase3CooldownGateClockValue = 0;
static int g_phase3CooldownGateRemainingSeconds = 0;
static int g_phase3CooldownLastLoggedRemainingSeconds = -1;
static Vehicle g_phase3PreparedVehicle = 0;
static Hash g_phase3PreparedModel = 0;
static int g_phase3PreparedMenuState = -999;
static int g_phase3PreparedSellPrice = 0;
static bool g_phase3PreparedSellPriceValid = false;
static bool g_phase3DisplayPriceEventPending = false;
static int g_phase3DisplayPriceOriginal = 0;
static int g_phase3DisplayPriceInjected = 0;

static Vehicle g_phase3FallbackVehicle = 0;
static Hash g_phase3FallbackModel = 0;
static int g_phase3FallbackPrice = 0;
static bool g_phase3FallbackLogged = false;
static bool g_phase3SellContextActive = false;
static int g_phase3SellContextPrice = 0;
static Phase3SellControlPath g_phase3SellControlPath{};
static Phase3SellStagePath g_phase3SellStagePath{};
static int g_phase3SellControlState = -1;
static int g_phase3LastLoggedSellControlState = -999;
static int g_phase3CurrentMenuState = -1;
static int g_phase3LastLoggedMenuState = -999;
static bool g_phase3SellStageActive = false;
static Phase2ThreadInfo g_phase3PriceThreadInfo{};
static bool g_phase3PriceThreadCached = false;
static ULONGLONG g_nextPhase3PriceUpdateAt = 0;
static constexpr ULONGLONG kPhase3PriceUpdateIntervalMs = 50ULL;

static void ResetPhase3PreparedSellPrice();
static bool IsPhase3PcAtNativeSite(
    uint32_t programCounter,
    uint32_t site);

static bool IsEnhancedEdition()
{
    return _stricmp(GetEditionName(), "Enhanced") == 0;
}

static bool IsLegacyEdition()
{
    return _stricmp(GetEditionName(), "Legacy") == 0;
}

static bool IsReadableMemory(const void* address, size_t size)
{
    if (!address || size == 0)
        return false;

    uintptr_t current = reinterpret_cast<uintptr_t>(address);
    const uintptr_t end = current + size;
    if (end < current)
        return false;

    while (current < end)
    {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) == 0)
            return false;

        if (info.State != MEM_COMMIT
            || (info.Protect & PAGE_GUARD) != 0
            || (info.Protect & PAGE_NOACCESS) != 0)
        {
            return false;
        }

        const uintptr_t regionEnd =
            reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;

        if (regionEnd <= current)
            return false;

        current = regionEnd < end ? regionEnd : end;
    }

    return true;
}

template <typename T>
static bool ReadMemoryValue(const void* base, size_t offset, T& value)
{
    if (!base)
        return false;

    const unsigned char* address =
        reinterpret_cast<const unsigned char*>(base) + offset;

    if (!IsReadableMemory(address, sizeof(T)))
        return false;

    std::memcpy(&value, address, sizeof(T));
    return true;
}

static unsigned char* FindPatternInRange(
    unsigned char* start,
    size_t size,
    const unsigned char* pattern,
    const char* mask)
{
    const size_t length = mask ? std::strlen(mask) : 0;
    if (!start || !pattern || length == 0 || size < length)
        return nullptr;

    for (size_t i = 0; i <= size - length; ++i)
    {
        bool match = true;
        for (size_t j = 0; j < length; ++j)
        {
            if (mask[j] == 'x' && start[i + j] != pattern[j])
            {
                match = false;
                break;
            }
        }

        if (match)
            return start + i;
    }

    return nullptr;
}

static unsigned char* FindExecutablePattern(
    const unsigned char* pattern,
    const char* mask)
{
    unsigned char* base =
        reinterpret_cast<unsigned char*>(GetModuleHandleA(nullptr));

    if (!base)
        return nullptr;

    const IMAGE_DOS_HEADER* dos =
        reinterpret_cast<const IMAGE_DOS_HEADER*>(base);

    if (!IsReadableMemory(dos, sizeof(*dos))
        || dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return nullptr;
    }

    const IMAGE_NT_HEADERS* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    if (!IsReadableMemory(nt, sizeof(*nt))
        || nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return nullptr;
    }

    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
    {
        if ((section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;

        unsigned char* sectionStart = base + section[i].VirtualAddress;
        const size_t sectionSize = static_cast<size_t>(section[i].Misc.VirtualSize);

        if (sectionSize == 0 || !IsReadableMemory(sectionStart, sectionSize))
            continue;

        unsigned char* match =
            FindPatternInRange(sectionStart, sectionSize, pattern, mask);

        if (match)
            return match;
    }

    return nullptr;
}

static void* ResolveRip(const unsigned char* displacement)
{
    if (!displacement || !IsReadableMemory(displacement, sizeof(int32_t)))
        return nullptr;

    int32_t relative = 0;
    std::memcpy(&relative, displacement, sizeof(relative));

    return const_cast<unsigned char*>(displacement) + 4 + relative;
}

static bool InitializePhase2Internals()
{
    if (g_phase2InternalsInitialized)
        return g_phase2ProgramResolverReady;

    g_phase2InternalsInitialized = true;

    if (!g_phase2Enabled)
    {
        g_phase2Status = "disabled in INI";
        Logf("[Phase2] Resolver skipped: feature disabled.");
        return false;
    }

    if (!IsLegacyEdition() && !IsEnhancedEdition())
    {
        g_phase2Status = "unsupported executable";
        Logf("[Phase2] Resolver failed safely: unsupported executable=%s", GetExecutableName());
        return false;
    }

    if (IsEnhancedEdition())
    {
        static const unsigned char programSig[] =
        {
            0x48, 0xC7, 0x84, 0xC8, 0xD8, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        static const unsigned char threadSig[] =
        {
            0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00,
            0x48, 0x89, 0x34, 0xF8, 0x48, 0xFF, 0xC7,
            0x48, 0x39, 0xFB, 0x75, 0x97
        };

        unsigned char* programMatch =
            FindExecutablePattern(programSig, "xxxxxxxxxxxx");
        unsigned char* threadMatch =
            FindExecutablePattern(threadSig, "xxx????xxxxxxxxxxxx");

        if (programMatch)
        {
            unsigned char* resolved =
                reinterpret_cast<unsigned char*>(ResolveRip(programMatch + 0x16));

            if (resolved)
                g_enhancedPrograms =
                    reinterpret_cast<Phase2ScrProgram**>(resolved + 0xD8);
        }

        if (threadMatch)
            g_threadArray =
                reinterpret_cast<ScrThreadArrayRaw*>(ResolveRip(threadMatch + 3));

        Logf(
            "[Phase2] Enhanced internals programPattern=%p programArray=%p threadPattern=%p threadArray=%p",
            programMatch,
            g_enhancedPrograms,
            threadMatch,
            g_threadArray);

        g_phase2ProgramResolverReady = g_enhancedPrograms != nullptr;
    }
    else
    {
        static const unsigned char programSig[] =
        {
            0x48, 0x03, 0x15, 0x00, 0x00, 0x00,
            0x00, 0x4C, 0x23, 0xC2, 0x49, 0x8B, 0x08
        };
        static const unsigned char threadSig[] =
        {
            0x45, 0x33, 0xF6, 0x8B, 0xE9, 0x85, 0xC9, 0xB8
        };

        unsigned char* programMatch =
            FindExecutablePattern(programSig, "xxx????xxxxxx");
        unsigned char* threadMatch =
            FindExecutablePattern(threadSig, "xxxxxxxx");

        if (programMatch)
            g_legacyScriptTable =
                reinterpret_cast<LegacyScriptTable*>(ResolveRip(programMatch + 3));

        if (threadMatch)
        {
            unsigned char* resolved =
                reinterpret_cast<unsigned char*>(ResolveRip(threadMatch - 4));

            if (resolved)
                g_threadArray =
                    reinterpret_cast<ScrThreadArrayRaw*>(resolved - 8);
        }

        Logf(
            "[Phase2] Legacy internals programPattern=%p scriptTable=%p threadPattern=%p threadArray=%p",
            programMatch,
            g_legacyScriptTable,
            threadMatch,
            g_threadArray);

        g_phase2ProgramResolverReady = g_legacyScriptTable != nullptr;
    }

    if (!g_phase2ProgramResolverReady)
    {
        g_phase2Status = "script program resolver failed";
        Logf(
            "[Phase2] Resolver FAILED safely. programResolver=no threadResolver=%s",
            g_threadArray ? "yes" : "no");
        return false;
    }

    g_phase2Status = g_threadArray
        ? "script program/thread resolvers ready"
        : "script program resolver ready; thread resolver unavailable";

    Logf(
        "[Phase2] Resolver READY edition=%s programResolver=yes threadResolver=%s. NETWORK_IS_GAME_IN_PROGRESS is untouched.",
        GetEditionName(),
        g_threadArray ? "yes" : "no");

    return true;
}

static Phase2ScrProgram* FindPhase2Program(uint32_t scriptHash)
{
    if (!InitializePhase2Internals())
        return nullptr;

    if (IsEnhancedEdition())
    {
        if (!IsReadableMemory(
                g_enhancedPrograms,
                sizeof(Phase2ScrProgram*) * kEnhancedProgramCount))
        {
            return nullptr;
        }

        for (int i = 0; i < kEnhancedProgramCount; ++i)
        {
            Phase2ScrProgram* program = g_enhancedPrograms[i];

            if (program
                && IsReadableMemory(program, sizeof(*program))
                && static_cast<uint32_t>(program->nameHash) == scriptHash)
            {
                return program;
            }
        }

        return nullptr;
    }

    if (!g_legacyScriptTable
        || !IsReadableMemory(g_legacyScriptTable, sizeof(*g_legacyScriptTable)))
    {
        return nullptr;
    }

    const int count = g_legacyScriptTable->count;
    LegacyScriptTableItem* table = g_legacyScriptTable->table;

    if (count < 0
        || count > 4096
        || !table
        || !IsReadableMemory(
            table,
            sizeof(LegacyScriptTableItem) * static_cast<size_t>(count)))
    {
        return nullptr;
    }

    for (int i = 0; i < count; ++i)
    {
        if (static_cast<uint32_t>(table[i].hash) != scriptHash)
            continue;

        Phase2ScrProgram* program = table[i].program;
        return program && IsReadableMemory(program, sizeof(*program))
            ? program
            : nullptr;
    }

    return nullptr;
}

static bool GetPhase2ThreadInfo(
    uint32_t scriptHash,
    Phase2ThreadInfo& info)
{
    info = Phase2ThreadInfo{};

    if (!InitializePhase2Internals()
        || !g_threadArray
        || !IsReadableMemory(g_threadArray, sizeof(*g_threadArray)))
    {
        return false;
    }

    const uint16_t count = g_threadArray->count;
    const uint16_t capacity = g_threadArray->capacity;
    void** data = g_threadArray->data;

    if (count == 0
        || count > capacity
        || count > 2048
        || !data
        || !IsReadableMemory(data, sizeof(void*) * static_cast<size_t>(count)))
    {
        return false;
    }

    const size_t stackOffset = IsEnhancedEdition() ? 0xB8 : 0xB0;
    const size_t hashOffset = IsEnhancedEdition() ? 0x150 : 0x128;
    const size_t stateOffset = IsEnhancedEdition() ? 0x18 : 0x10;
    const size_t pcOffset = IsEnhancedEdition() ? 0x1C : 0x14;
    const size_t framePointerOffset = IsEnhancedEdition() ? 0x20 : 0x18;
    const size_t stackPointerOffset = IsEnhancedEdition() ? 0x24 : 0x1C;
    const size_t stackSizeOffset = IsEnhancedEdition() ? 0x60 : 0x58;

    for (uint16_t i = 0; i < count; ++i)
    {
        void* thread = data[i];
        uint32_t threadId = 0;
        uint32_t threadHash = 0;

        if (!thread
            || !ReadMemoryValue(thread, 0x08, threadId)
            || threadId == 0
            || !ReadMemoryValue(thread, hashOffset, threadHash)
            || threadHash != scriptHash)
        {
            continue;
        }

        if (IsLegacyEdition())
        {
            void* handler = nullptr;
            if (!ReadMemoryValue(thread, 0x118, handler) || !handler)
                continue;
        }

        info.thread = thread;
        info.threadId = threadId;
        ReadMemoryValue(thread, stackOffset, info.stack);
        ReadMemoryValue(thread, stateOffset, info.threadState);
        ReadMemoryValue(thread, pcOffset, info.programCounter);
        ReadMemoryValue(thread, framePointerOffset, info.framePointer);
        ReadMemoryValue(thread, stackPointerOffset, info.stackPointer);
        ReadMemoryValue(thread, stackSizeOffset, info.stackSize);
        return true;
    }

    return false;
}

static bool ValidateProgramCode(Phase2ScrProgram* program)
{
    if (!program
        || !IsReadableMemory(program, sizeof(*program))
        || program->codeSize <= 0
        || program->codeSize > 16 * 1024 * 1024
        || !program->codeBlocks)
    {
        return false;
    }

    const size_t pageCount =
        (static_cast<size_t>(program->codeSize) + 0x3FFF) >> 14;

    if (pageCount == 0
        || pageCount > 1024
        || !IsReadableMemory(
            program->codeBlocks,
            sizeof(unsigned char*) * pageCount))
    {
        return false;
    }

    for (size_t i = 0; i < pageCount; ++i)
    {
        if (!program->codeBlocks[i]
            || !IsReadableMemory(program->codeBlocks[i], 1))
        {
            return false;
        }
    }

    return true;
}

static unsigned char* ScriptCodePointer(
    Phase2ScrProgram* program,
    uint32_t position)
{
    if (!program
        || position >= static_cast<uint32_t>(program->codeSize))
    {
        return nullptr;
    }

    return program->codeBlocks[position >> 14] + (position & 0x3FFF);
}

static bool ReadScriptUnsigned(
    Phase2ScrProgram* program,
    uint32_t position,
    unsigned int byteCount,
    uint32_t& value)
{
    value = 0;

    for (unsigned int i = 0; i < byteCount; ++i)
    {
        unsigned char* byte = ScriptCodePointer(program, position + i);
        if (!byte)
            return false;

        value |= static_cast<uint32_t>(*byte) << (i * 8);
    }

    return true;
}

static bool GetVmInstructionLength(
    Phase2ScrProgram* program,
    uint32_t position,
    uint32_t& length)
{
    unsigned char* opPtr = ScriptCodePointer(program, position);
    if (!opPtr)
        return false;

    const unsigned char op = *opPtr;

    if (op <= 0x24) length = 1;
    else if (op == 0x25) length = 2;
    else if (op == 0x26) length = 3;
    else if (op == 0x27) length = 4;
    else if (op == 0x28 || op == 0x29) length = 5;
    else if (op == 0x2A || op == 0x2B) length = 1;
    else if (op == 0x2C) length = 4;
    else if (op == kVmEnter)
    {
        unsigned char* skip = ScriptCodePointer(program, position + 4);
        if (!skip)
            return false;
        length = 5 + *skip;
    }
    else if (op == kVmLeave) length = 3;
    else if (op >= 0x2F && op <= 0x33) length = 1;
    else if (op >= 0x34 && op <= 0x3E) length = 2;
    else if (op == 0x3F) length = 1;
    else if (op >= 0x40 && op <= 0x42) length = 2;
    else if (op >= 0x43 && op <= 0x5C) length = 3;
    else if (op >= 0x5D && op <= 0x64) length = 4;
    else if (op == kVmSwitch)
    {
        unsigned char* entries = ScriptCodePointer(program, position + 1);
        if (!entries)
            return false;
        length = 2 + static_cast<uint32_t>(*entries) * 6;
    }
    else if (op == 0x66 || op == 0x67) length = 1;
    else if (op >= 0x68 && op <= 0x6B) length = 2;
    else if (op >= 0x6C && op <= 0x82) length = 1;
    else return false;

    const uint32_t codeSize = static_cast<uint32_t>(program->codeSize);
    return length > 0 && position <= codeSize && length <= codeSize - position;
}

static bool BuildVmFunctionCatalog(
    Phase2ScrProgram* program,
    std::vector<VmFunctionRange>& functions)
{
    functions.clear();

    uint32_t position = 0;
    int functionIndex = 0;

    while (position < static_cast<uint32_t>(program->codeSize))
    {
        unsigned char* opPtr = ScriptCodePointer(program, position);
        uint32_t length = 0;

        if (!opPtr || !GetVmInstructionLength(program, position, length))
        {
            Logf(
                "[Phase2] VM parser failed code=0x%X opcode=0x%02X",
                position,
                opPtr ? static_cast<unsigned int>(*opPtr) : 0U);
            return false;
        }

        if (*opPtr == kVmEnter)
        {
            if (!functions.empty())
                functions.back().end = position;

            unsigned char* argCount = ScriptCodePointer(program, position + 1);
            if (!argCount)
                return false;

            VmFunctionRange function{};
            function.index = functionIndex++;
            function.start = position;
            function.end = 0;
            function.argCount = *argCount;
            function.found = true;
            functions.push_back(function);
        }

        position += length;
    }

    if (!functions.empty())
        functions.back().end = static_cast<uint32_t>(program->codeSize);

    Logf(
        "[Phase2] FunctionCatalog count=%u",
        static_cast<unsigned int>(functions.size()));

    return !functions.empty();
}

static const VmFunctionRange* FindVmFunctionByStart(
    const std::vector<VmFunctionRange>& functions,
    uint32_t start)
{
    for (size_t i = 0; i < functions.size(); ++i)
    {
        if (functions[i].start == start)
            return &functions[i];
    }

    return nullptr;
}

static bool TryGetVmPushedInt(
    Phase2ScrProgram* program,
    uint32_t position,
    int& value)
{
    unsigned char* opPtr = ScriptCodePointer(program, position);
    if (!opPtr)
        return false;

    uint32_t raw = 0;

    if (*opPtr == kVmPushConstU8)
    {
        if (!ReadScriptUnsigned(program, position + 1, 1, raw))
            return false;
        value = static_cast<int>(raw);
        return true;
    }

    if (*opPtr == kVmPushConstS16)
    {
        if (!ReadScriptUnsigned(program, position + 1, 2, raw))
            return false;
        value = static_cast<int>(static_cast<int16_t>(raw));
        return true;
    }

    if (*opPtr == kVmPushConstU24)
    {
        if (!ReadScriptUnsigned(program, position + 1, 3, raw))
            return false;
        value = static_cast<int>(raw);
        return true;
    }

    if (*opPtr == kVmPushConstU32)
    {
        if (!ReadScriptUnsigned(program, position + 1, 4, raw))
            return false;
        value = static_cast<int>(raw);
        return true;
    }

    if (*opPtr >= 0x70 && *opPtr <= 0x78)
    {
        value = *opPtr == 0x70
            ? -1
            : static_cast<int>(*opPtr - 0x71);
        return true;
    }

    return false;
}

static bool IsVmIntegerComparison(unsigned char op)
{
    return op == kVmIeq
        || op == kVmIne
        || op == kVmIeqJz
        || op == kVmIneJz;
}

static bool VmReferencesLocalIndex(
    Phase2ScrProgram* program,
    uint32_t position,
    uint32_t expected,
    bool loadOnly)
{
    unsigned char* opPtr = ScriptCodePointer(program, position);
    if (!opPtr)
        return false;

    uint32_t value = 0;

    if (*opPtr == kVmLocalU8Load || (!loadOnly && *opPtr == kVmLocalU8))
        return ReadScriptUnsigned(program, position + 1, 1, value)
            && value == expected;

    if (*opPtr == kVmLocalU16Load || (!loadOnly && *opPtr == kVmLocalU16))
        return ReadScriptUnsigned(program, position + 1, 2, value)
            && value == expected;

    if (*opPtr == kVmLocalU24Load || (!loadOnly && *opPtr == kVmLocalU24))
        return ReadScriptUnsigned(program, position + 1, 3, value)
            && value == expected;

    return false;
}

struct VmCategoryCondition
{
    int value;
    uint32_t valuePosition;
    uint32_t comparisonPosition;
};

static bool CollectParamZeroCategoryConditions(
    Phase2ScrProgram* program,
    const VmFunctionRange& function,
    std::vector<VmCategoryCondition>& conditions)
{
    conditions.clear();

    uint32_t previous = 0xFFFFFFFFU;
    uint32_t previousPrevious = 0xFFFFFFFFU;

    for (uint32_t position = function.start; position < function.end;)
    {
        unsigned char* opPtr = ScriptCodePointer(program, position);
        uint32_t length = 0;

        if (!opPtr || !GetVmInstructionLength(program, position, length))
            return false;

        int pushedValue = 0;
        if (TryGetVmPushedInt(program, position, pushedValue))
        {
            const bool previousLoadsParam0 =
                previous != 0xFFFFFFFFU
                && (VmReferencesLocalIndex(program, previous, 0, true)
                    || (previousPrevious != 0xFFFFFFFFU
                        && *ScriptCodePointer(program, previous) == kVmLoad
                        && VmReferencesLocalIndex(program, previousPrevious, 0, false)));

            uint32_t next = position + length;
            unsigned char* nextOp = ScriptCodePointer(program, next);
            uint32_t comparisonPosition = 0;

            if (previousLoadsParam0
                && nextOp
                && IsVmIntegerComparison(*nextOp))
            {
                comparisonPosition = next;
            }
            else if (nextOp && VmReferencesLocalIndex(program, next, 0, true))
            {
                uint32_t nextLength = 0;
                if (!GetVmInstructionLength(program, next, nextLength))
                    return false;

                unsigned char* comparison =
                    ScriptCodePointer(program, next + nextLength);

                if (comparison && IsVmIntegerComparison(*comparison))
                    comparisonPosition = next + nextLength;
            }

            if (comparisonPosition)
            {
                VmCategoryCondition condition{};
                condition.value = pushedValue;
                condition.valuePosition = position;
                condition.comparisonPosition = comparisonPosition;
                conditions.push_back(condition);
            }
        }

        previousPrevious = previous;
        previous = position;
        position += length;
    }

    return true;
}

static bool ReadVmNativeSignature(
    Phase2ScrProgram* program,
    uint32_t position,
    uint8_t& packedArgsReturns,
    uint16_t& tableIndex)
{
    unsigned char* op = ScriptCodePointer(program, position);
    unsigned char* packed = ScriptCodePointer(program, position + 1);
    unsigned char* high = ScriptCodePointer(program, position + 2);
    unsigned char* low = ScriptCodePointer(program, position + 3);

    if (!op || !packed || !high || !low || *op != kVmNative)
        return false;

    packedArgsReturns = *packed;
    tableIndex = static_cast<uint16_t>(
        (static_cast<uint16_t>(*high) << 8)
        | static_cast<uint16_t>(*low));
    return true;
}

static bool ValidateSellFilterShape(
    Phase2ScrProgram* program,
    const VmFunctionRange& function,
    uint16_t& repeatedNativeIndex)
{
    repeatedNativeIndex = 0xFFFF;

    if (function.argCount != 1
        || function.end <= function.start
        || function.end - function.start < 96)
    {
        return false;
    }

    uint32_t position = function.start;
    uint32_t length = 0;
    if (!GetVmInstructionLength(program, position, length))
        return false;

    position += length;

    uint16_t firstNativeIndex = 0xFFFF;
    bool firstNativeSeen = false;
    const uint32_t scanEnd =
        function.end < position + 160 ? function.end : position + 160;

    while (position < scanEnd)
    {
        unsigned char* opPtr = ScriptCodePointer(program, position);
        if (!opPtr || !GetVmInstructionLength(program, position, length))
            return false;

        if (*opPtr == kVmNative)
        {
            uint8_t packed = 0;
            uint16_t index = 0;
            if (!ReadVmNativeSignature(program, position, packed, index))
                return false;

            // NETWORK_IS_GAME_IN_PROGRESS takes zero arguments and returns one
            // value. The Sell filter calls it twice right at the start.
            if (packed == 0x01)
            {
                if (!firstNativeSeen)
                {
                    firstNativeSeen = true;
                    firstNativeIndex = index;
                }
                else if (index == firstNativeIndex)
                {
                    repeatedNativeIndex = index;
                    return true;
                }
            }
        }

        position += length;
    }

    return false;
}

static bool FindSellVisibilityCall(
    Phase2ScrProgram* program,
    const std::vector<VmFunctionRange>& functions,
    VmFunctionRange& visibility,
    VmFunctionRange& sellFilter,
    uint32_t& callPosition)
{
    callPosition = 0;
    visibility = VmFunctionRange{};
    sellFilter = VmFunctionRange{};

    int candidateCount = 0;

    for (size_t functionIndex = 0;
         functionIndex < functions.size();
         ++functionIndex)
    {
        const VmFunctionRange& function = functions[functionIndex];
        if (function.argCount != 1 || function.end <= function.start)
            continue;

        std::vector<VmCategoryCondition> conditions;
        if (!CollectParamZeroCategoryConditions(program, function, conditions))
            return false;

        for (size_t i = 2; i + 2 < conditions.size(); ++i)
        {
            // Rockstar's category-availability chain around Sell is:
            // ... 55, 31, 42, 47, 27 ...
            // Matching the neighboring parameter comparisons avoids relying on
            // decompiler function numbers or on a generic constant-42 search.
            if (conditions[i - 2].value != 55
                || conditions[i - 1].value != 31
                || conditions[i].value != 42
                || conditions[i + 1].value != 47
                || conditions[i + 2].value != 27)
            {
                continue;
            }

            int callsBetween42And47 = 0;
            uint32_t candidateCall = 0;
            uint32_t candidateTarget = 0;

            uint32_t position = conditions[i].comparisonPosition;
            while (position < conditions[i + 1].valuePosition)
            {
                unsigned char* opPtr = ScriptCodePointer(program, position);
                uint32_t length = 0;
                if (!opPtr || !GetVmInstructionLength(program, position, length))
                    return false;

                if (*opPtr == kVmCall)
                {
                    uint32_t target = 0;
                    if (!ReadScriptUnsigned(program, position + 1, 3, target))
                        return false;

                    ++callsBetween42And47;
                    candidateCall = position;
                    candidateTarget = target;
                }

                position += length;
            }

            const VmFunctionRange* targetFunction =
                callsBetween42And47 == 1
                    ? FindVmFunctionByStart(functions, candidateTarget)
                    : nullptr;

            uint16_t repeatedNativeIndex = 0xFFFF;
            const bool filterShapeMatches =
                targetFunction
                && ValidateSellFilterShape(
                    program,
                    *targetFunction,
                    repeatedNativeIndex);

            Logf(
                "[Phase2] StructuralCandidate visibilityFunc=%d@0x%X category42=0x%X callsBefore47=%d call=0x%X target=0x%X targetFunc=%d targetArgs=%u repeatedNative=%s nativeIndex=%u qualified=%s",
                function.index,
                function.start,
                conditions[i].valuePosition,
                callsBetween42And47,
                candidateCall,
                candidateTarget,
                targetFunction ? targetFunction->index : -1,
                targetFunction
                    ? static_cast<unsigned int>(targetFunction->argCount)
                    : 0U,
                repeatedNativeIndex != 0xFFFF ? "yes" : "no",
                repeatedNativeIndex != 0xFFFF
                    ? static_cast<unsigned int>(repeatedNativeIndex)
                    : 0U,
                filterShapeMatches ? "yes" : "no");

            if (!filterShapeMatches)
                continue;

            ++candidateCount;
            visibility = function;
            sellFilter = *targetFunction;
            callPosition = candidateCall;
        }
    }

    Logf(
        "[Phase2] Structural resolution qualifiedCandidates=%d",
        candidateCount);

    return candidateCount == 1;
}

static bool WriteVmPatch(
    Phase2ScrProgram* program,
    uint32_t position,
    const unsigned char patch[4])
{
    if ((position >> 14) != ((position + 3) >> 14))
        return false;

    unsigned char* address = ScriptCodePointer(program, position);
    if (!address || !IsReadableMemory(address, 4))
        return false;

    DWORD oldProtection = 0;
    if (!VirtualProtect(address, 4, PAGE_EXECUTE_READWRITE, &oldProtection))
    {
        Logf(
            "[Phase2] VirtualProtect failed error=%lu",
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    std::memcpy(address, patch, 4);
    FlushInstructionCache(GetCurrentProcess(), address, 4);

    DWORD ignored = 0;
    VirtualProtect(address, 4, oldProtection, &ignored);

    return std::memcmp(address, patch, 4) == 0;
}

static bool ApplySellVisibilityPatch(Phase2ScrProgram* program)
{
    if (!ValidateProgramCode(program))
    {
        g_phase2Status = "invalid carmod_shop program";
        Logf("[Phase2] SellExposed=no reason=invalid program/code layout");
        return false;
    }

    Logf(
        "[Phase2] Program found program=%p nameHash=0x%08X codeSize=%d localCount=%d nativeCount=%d",
        program,
        static_cast<unsigned int>(program->nameHash),
        program->codeSize,
        program->localCount,
        program->nativeCount);

    std::vector<VmFunctionRange> functions;
    if (!BuildVmFunctionCatalog(program, functions))
    {
        g_phase2Status = "function catalog failed";
        Logf("[Phase2] SellExposed=no reason=function catalog failed");
        return false;
    }

    VmFunctionRange visibility{};
    VmFunctionRange sellFilter{};
    uint32_t callPosition = 0;

    if (!FindSellVisibilityCall(
            program,
            functions,
            visibility,
            sellFilter,
            callPosition))
    {
        g_phase2Status = "structural Sell gate not uniquely resolved";
        Logf("[Phase2] SellExposed=no reason=structural Sell gate not uniquely resolved");
        return false;
    }

    uint16_t networkGameNativeIndex = 0xFFFF;
    if (!ValidateSellFilterShape(
            program,
            sellFilter,
            networkGameNativeIndex))
    {
        g_phase2Status = "Sell filter revalidation failed";
        Logf("[Phase2] SellExposed=no reason=Sell filter revalidation failed");
        return false;
    }

    g_phase2NetworkGameNativeIndex = networkGameNativeIndex;

    unsigned char* call = ScriptCodePointer(program, callPosition);
    uint32_t target = 0;

    if (!call
        || call[0] != kVmCall
        || !ReadScriptUnsigned(program, callPosition + 1, 3, target)
        || target != sellFilter.start)
    {
        g_phase2Status = "target call validation failed";
        Logf("[Phase2] SellExposed=no reason=target call validation failed");
        return false;
    }

    const unsigned char original[4] =
    {
        call[0], call[1], call[2], call[3]
    };

    std::memcpy(
        g_phase2OriginalCall,
        original,
        sizeof(g_phase2OriginalCall));

    // The structurally resolved Sell filter takes one vehicle argument and
    // returns one bool. Replacing only this call with DROP + false keeps the
    // VM stack balanced while bypassing the Story Mode hide decision for
    // category 42 alone.
    const unsigned char patch[4] =
    {
        kVmDrop, kVmPushConst0, kVmNop, kVmNop
    };

    std::memcpy(
        g_phase2VisibilityPatch,
        patch,
        sizeof(g_phase2VisibilityPatch));

    if (!WriteVmPatch(program, callPosition, patch))
    {
        g_phase2Status = "VM patch write failed";
        Logf("[Phase2] SellExposed=no reason=VM patch write/verify failed");
        return false;
    }

    g_phase2PatchedProgram = program;
    g_phase2PatchPosition = callPosition;
    g_phase2PatchApplied = true;
    g_phase2VisibilityBypassActive = true;
    g_phase2Status = "Sell visibility gate bypassed";

    Phase2ThreadInfo threadInfo{};
    const bool threadFound = GetPhase2ThreadInfo(kCarmodShopHash, threadInfo);

    Logf(
        "[Phase2] SellExposed=yes method=structural-category42-call-bypass program=%p visibilityFunc=%d@0x%X sellFilterFunc=%d@0x%X call=0x%X original=%02X %02X %02X %02X patch=%02X %02X %02X %02X runningThread=%s pc=0x%X",
        program,
        visibility.index,
        visibility.start,
        sellFilter.index,
        sellFilter.start,
        callPosition,
        original[0],
        original[1],
        original[2],
        original[3],
        patch[0],
        patch[1],
        patch[2],
        patch[3],
        threadFound ? "yes" : "no",
        threadFound ? threadInfo.programCounter : 0U);

    Logf(
        "[Phase2] Scope: one carmod_shop ScriptVM CALL only. No NETWORK native spoof, script-local write, vehicle write, or money write.");

    return true;
}

static bool PlateTextEquals(
    Vehicle vehicle,
    const char* expected)
{
    if (vehicle == 0
        || !expected)
    {
        return false;
    }

    const char* plate =
        VEHICLE::GET_VEHICLE_NUMBER_PLATE_TEXT(
            vehicle);

    if (!plate)
        return false;

    char normalized[16]{};
    strncpy_s(
        normalized,
        sizeof(normalized),
        plate,
        _TRUNCATE);

    size_t length =
        strnlen_s(
            normalized,
            sizeof(normalized));

    while (length > 0
        && normalized[length - 1] == ' ')
    {
        normalized[--length] = '\0';
    }

    return _stricmp(
        normalized,
        expected) == 0;
}

static bool IsRockstarCharacterVehicle(
    Vehicle vehicle)
{
    if (vehicle == 0
        || !ENTITY::DOES_ENTITY_EXIST(vehicle))
    {
        return false;
    }

    const Hash model =
        ENTITY::GET_ENTITY_MODEL(vehicle);

    // Rockstar's SP player vehicle data identifies the protagonist vehicles
    // with model + fixed plate combinations. Using both avoids blocking an
    // ordinary traffic vehicle merely because it shares the same model.
    if (model == Joaat("tailgater"))
        return PlateTextEquals(vehicle, "5MDS003");

    if (model == Joaat("premier"))
        return PlateTextEquals(vehicle, "880HS955");

    if (model == Joaat("bodhi2"))
        return PlateTextEquals(vehicle, "BETTY 32");

    if (model == Joaat("buffalo2"))
        return PlateTextEquals(vehicle, "FC1988");

    if (model == Joaat("bagger"))
        return PlateTextEquals(vehicle, "FC88");

    return false;
}

static bool SetPhase2SellVisibilityBypass(
    bool enabled,
    const char* reason)
{
    if (!g_phase2PatchApplied
        || !g_phase2PatchedProgram
        || g_phase2PatchPosition == 0)
    {
        return false;
    }

    if (g_phase2VisibilityBypassActive
        == enabled)
    {
        return true;
    }

    const unsigned char* bytes =
        enabled
            ? g_phase2VisibilityPatch
            : g_phase2OriginalCall;

    if (!WriteVmPatch(
            g_phase2PatchedProgram,
            g_phase2PatchPosition,
            bytes))
    {
        Logf(
            "[Gameplay] CharacterVehicle Sell visibility toggle failed enabled=%s reason=%s",
            enabled ? "yes" : "no",
            reason ? reason : "unspecified");
        return false;
    }

    g_phase2VisibilityBypassActive =
        enabled;

    Logf(
        "[Gameplay] CharacterVehicle Sell visibility exposed=%s reason=%s",
        enabled ? "yes" : "no",
        reason ? reason : "unspecified");

    return true;
}

static void ApplyCharacterVehicleSettingForShop()
{
    if (!g_phase2PatchApplied
        || g_lastNetworkGame)
    {
        return;
    }

    const Ped playerPed =
        PLAYER::PLAYER_PED_ID();

    if (!PED::IS_PED_IN_ANY_VEHICLE(
            playerPed,
            false))
    {
        // Fail open for ordinary LSC behavior if no current vehicle can be
        // identified. This also repairs a stale hidden state from a previous
        // session if restoring the patch had failed for any reason.
        SetPhase2SellVisibilityBypass(
            true,
            "no current shop vehicle");
        return;
    }

    const Vehicle vehicle =
        PED::GET_VEHICLE_PED_IS_IN(
            playerPed,
            false);

    const bool characterVehicle =
        IsRockstarCharacterVehicle(vehicle);

    const bool exposeSell =
        g_allowCharacterVehicles
        || !characterVehicle;

    SetPhase2SellVisibilityBypass(
        exposeSell,
        characterVehicle
            ? (g_allowCharacterVehicles
                ? "character vehicle allowed"
                : "AllowCharacterVehicles=false")
            : "non-character vehicle");
}

static void RestoreCharacterVehicleSettingAfterShop()
{
    if (!g_allowCharacterVehicles
        && g_phase2PatchApplied
        && !g_phase2VisibilityBypassActive)
    {
        SetPhase2SellVisibilityBypass(
            true,
            "shop ended");
    }
}

static void UpdatePhase2SellExposure()
{
    g_stockLscScopeActive =
        IsPlayerNearStockLsc();

    if (!g_stockLscScopeActive)
    {
        RestoreStockLscPatches(
            "outside stock Los Santos Customs");
        return;
    }

    if (!g_phase2Enabled
        || g_lastNetworkGame
        || !InitializePhase2Internals())
    {
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (now < g_nextPhase2ProgramCheckAt)
        return;

    g_nextPhase2ProgramCheckAt =
        now + (g_phase2PatchApplied ? 1000ULL : 50ULL);

    Phase2ScrProgram* program = FindPhase2Program(kCarmodShopHash);

    if (program != g_phase2ObservedProgram)
    {
        if (g_phase2ObservedProgram)
        {
            Logf(
                "[Phase2] carmod_shop program changed/unloaded old=%p new=%p",
                g_phase2ObservedProgram,
                program);
        }

        g_phase2ObservedProgram = program;
        g_phase2AttemptedProgram = nullptr;

        if (program != g_phase3AnalyzedProgram)
        {
            g_phase3AnalyzedProgram = nullptr;
            g_phase3FunctionCatalog.clear();
            g_phase3SellHandler = VmFunctionRange{};
            g_phase3SellHandlerResolved = false;
            g_phase3TraceUntil = 0;
            g_phase2NetworkGameNativeIndex = 0xFFFF;
            g_phase3SellPricePath = Phase3SellPricePath{};
            g_phase3SellDisplayPriceHook =
                Phase3SellDisplayPriceHook{};
            g_phase3SellCooldownPath =
                Phase3SellCooldownPath{};
            g_phase3SellCooldownHook =
                Phase3SellCooldownHook{};
            g_phase3CooldownGateEventPending = false;
            g_phase3CooldownGateClockValue = 0;
            g_phase3CooldownGateRemainingSeconds = 0;
            g_phase3CooldownLastLoggedRemainingSeconds = -1;
            ResetPhase3PreparedSellPrice();
            g_phase3DisplayPriceEventPending = false;
            g_phase3SellControlPath = Phase3SellControlPath{};
            g_phase3SellStagePath = Phase3SellStagePath{};
            g_phase3SellControlState = -1;
            g_phase3LastLoggedSellControlState = -999;
            g_phase3CurrentMenuState = -1;
            g_phase3LastLoggedMenuState = -999;
            g_phase3SellStageActive = false;
            g_phase3PriceThreadInfo = Phase2ThreadInfo{};
            g_phase3PriceThreadCached = false;
            g_nextPhase3PriceUpdateAt = 0;
            g_phase3SellEligibilityFunction = VmFunctionRange{};
            g_phase3NoSell1MessagePush = 0;
            g_phase3PlayerOwnedHelper = VmFunctionRange{};

            if (program != g_highValueSellPatchedProgram)
            {
                g_highValueSellPatchedProgram = nullptr;
                g_highValueSellPatchPosition = 0;
                g_highValueSellPatchApplied = false;
                std::memset(
                    g_highValueSellOriginal,
                    0,
                    sizeof(g_highValueSellOriginal));
            }

            if (program != g_sellOwnershipPatchedProgram)
            {
                g_sellOwnershipPatchedProgram = nullptr;
                g_sellOwnershipPatchPositions.clear();
                g_sellOwnershipPatchBackups.clear();
                g_sellOwnershipPatchApplied = false;
            }
        }

        if (g_phase2PatchedProgram && program != g_phase2PatchedProgram)
        {
            g_phase2PatchedProgram = nullptr;
            g_phase2PatchPosition = 0;
            g_phase2PatchApplied = false;
            g_phase2VisibilityBypassActive = false;
            std::memset(
                g_phase2OriginalCall,
                0,
                sizeof(g_phase2OriginalCall));
            std::memset(
                g_phase2VisibilityPatch,
                0,
                sizeof(g_phase2VisibilityPatch));
        }
    }

    if (!program
        || g_phase2PatchApplied
        || g_phase2AttemptedProgram == program)
    {
        return;
    }

    g_phase2AttemptedProgram = program;

    if (!ApplySellVisibilityPatch(program))
    {
        Logf(
            "[Phase2] Patch attempt FAILED safely program=%p status=%s",
            program,
            g_phase2Status.c_str());
    }
    else
    {
        // Apply the character-vehicle policy as soon as the structural Sell
        // call is resolved, before carmod_shop has a chance to build its root
        // menu. BeginCarmodShopSession repeats this check as a cheap safeguard.
        ApplyCharacterVehicleSettingForShop();
    }
}

static bool ValidateProgramStrings(Phase2ScrProgram* program)
{
    if (!program
        || !IsReadableMemory(program, sizeof(*program))
        || program->stringSize <= 0
        || program->stringSize > 16 * 1024 * 1024
        || !program->stringsOffset)
    {
        return false;
    }

    const size_t pageCount =
        (static_cast<size_t>(program->stringSize) + 0x3FFF) >> 14;

    if (pageCount == 0
        || pageCount > 1024
        || !IsReadableMemory(
            program->stringsOffset,
            sizeof(char*) * pageCount))
    {
        return false;
    }

    for (size_t i = 0; i < pageCount; ++i)
    {
        const size_t pageStart = i << 14;
        const size_t remaining =
            static_cast<size_t>(program->stringSize) - pageStart;
        const size_t pageSize = remaining < 0x4000 ? remaining : 0x4000;

        if (!program->stringsOffset[i]
            || pageSize == 0
            || !IsReadableMemory(program->stringsOffset[i], pageSize))
        {
            return false;
        }
    }

    return true;
}

static char* ScriptStringPointer(
    Phase2ScrProgram* program,
    uint32_t position)
{
    if (!program
        || position >= static_cast<uint32_t>(program->stringSize))
    {
        return nullptr;
    }

    return program->stringsOffset[position >> 14] + (position & 0x3FFF);
}

static bool ReadScriptStringByte(
    Phase2ScrProgram* program,
    uint32_t position,
    unsigned char& value)
{
    char* byte = ScriptStringPointer(program, position);
    if (!byte)
        return false;

    value = static_cast<unsigned char>(*byte);
    return true;
}

static bool ScriptStringEqualsAt(
    Phase2ScrProgram* program,
    uint32_t position,
    const char* text)
{
    if (!program || !text)
        return false;

    if (position != 0)
    {
        unsigned char previous = 0;
        if (!ReadScriptStringByte(program, position - 1, previous)
            || previous != 0)
        {
            return false;
        }
    }

    const size_t length = std::strlen(text);
    if (length == 0
        || position + length >= static_cast<uint32_t>(program->stringSize))
    {
        return false;
    }

    for (size_t i = 0; i < length; ++i)
    {
        unsigned char value = 0;
        if (!ReadScriptStringByte(
                program,
                position + static_cast<uint32_t>(i),
                value)
            || value != static_cast<unsigned char>(text[i]))
        {
            return false;
        }
    }

    unsigned char terminator = 0xFF;
    return ReadScriptStringByte(
            program,
            position + static_cast<uint32_t>(length),
            terminator)
        && terminator == 0;
}

static void FindExactScriptStringOffsets(
    Phase2ScrProgram* program,
    const char* text,
    std::vector<uint32_t>& offsets)
{
    offsets.clear();

    if (!ValidateProgramStrings(program) || !text || !*text)
        return;

    const uint32_t stringSize = static_cast<uint32_t>(program->stringSize);
    for (uint32_t position = 0; position < stringSize; ++position)
    {
        unsigned char first = 0;
        if (!ReadScriptStringByte(program, position, first))
            return;

        if (first == static_cast<unsigned char>(text[0])
            && ScriptStringEqualsAt(program, position, text))
        {
            offsets.push_back(position);
        }
    }
}

struct Phase3SellAnchor
{
    const char* label = nullptr;
    std::vector<uint32_t> offsets;
};

struct Phase3StringReference
{
    int anchorIndex;
    uint32_t pushPosition;
    uint32_t stringPosition;
    uint32_t stringOffset;
};

static bool OffsetMatchesAnchor(
    uint32_t value,
    const Phase3SellAnchor& anchor)
{
    for (size_t i = 0; i < anchor.offsets.size(); ++i)
    {
        if (anchor.offsets[i] == value)
            return true;
    }

    return false;
}

static bool CollectSellStringReferences(
    Phase2ScrProgram* program,
    const VmFunctionRange& function,
    const std::vector<Phase3SellAnchor>& anchors,
    std::vector<Phase3StringReference>& references,
    int& distinctAnchorCount)
{
    references.clear();
    distinctAnchorCount = 0;

    if (anchors.empty())
        return false;

    std::vector<bool> seen(anchors.size(), false);

    for (uint32_t position = function.start; position < function.end;)
    {
        uint32_t length = 0;
        unsigned char* op = ScriptCodePointer(program, position);
        if (!op || !GetVmInstructionLength(program, position, length))
            return false;

        int pushedValue = -1;
        if (TryGetVmPushedInt(program, position, pushedValue)
            && pushedValue >= 0)
        {
            const uint32_t stringPosition = position + length;
            unsigned char* stringOp =
                ScriptCodePointer(program, stringPosition);

            if (stringOp && *stringOp == kVmString)
            {
                const uint32_t offset =
                    static_cast<uint32_t>(pushedValue);

                for (size_t anchorIndex = 0;
                     anchorIndex < anchors.size();
                     ++anchorIndex)
                {
                    if (!OffsetMatchesAnchor(offset, anchors[anchorIndex]))
                        continue;

                    Phase3StringReference reference{};
                    reference.anchorIndex = static_cast<int>(anchorIndex);
                    reference.pushPosition = position;
                    reference.stringPosition = stringPosition;
                    reference.stringOffset = offset;
                    references.push_back(reference);

                    if (!seen[anchorIndex])
                    {
                        seen[anchorIndex] = true;
                        ++distinctAnchorCount;
                    }
                }
            }
        }

        position += length;
    }

    return true;
}

static const VmFunctionRange* FindVmFunctionContaining(
    const std::vector<VmFunctionRange>& functions,
    uint32_t position)
{
    for (size_t i = 0; i < functions.size(); ++i)
    {
        if (position >= functions[i].start
            && position < functions[i].end)
        {
            return &functions[i];
        }
    }

    return nullptr;
}

static int CountNativeIndexReferences(
    Phase2ScrProgram* program,
    const VmFunctionRange& function,
    uint16_t nativeIndex,
    std::vector<uint32_t>* positions)
{
    if (positions)
        positions->clear();

    int count = 0;

    for (uint32_t position = function.start; position < function.end;)
    {
        uint32_t length = 0;
        unsigned char* op = ScriptCodePointer(program, position);
        if (!op || !GetVmInstructionLength(program, position, length))
            return -1;

        if (*op == kVmNative)
        {
            uint8_t packed = 0;
            uint16_t index = 0;
            if (!ReadVmNativeSignature(program, position, packed, index))
                return -1;

            if (index == nativeIndex)
            {
                ++count;
                if (positions)
                    positions->push_back(position);
            }
        }

        position += length;
    }

    return count;
}

struct Phase3DirectCall
{
    uint32_t callPosition;
    uint32_t target;
    const VmFunctionRange* targetFunction;
};

static bool CollectDirectCalls(
    Phase2ScrProgram* program,
    const VmFunctionRange& function,
    const std::vector<VmFunctionRange>& functions,
    std::vector<Phase3DirectCall>& calls)
{
    calls.clear();

    for (uint32_t position = function.start; position < function.end;)
    {
        uint32_t length = 0;
        unsigned char* op = ScriptCodePointer(program, position);
        if (!op || !GetVmInstructionLength(program, position, length))
            return false;

        if (*op == kVmCall)
        {
            uint32_t target = 0;
            if (!ReadScriptUnsigned(program, position + 1, 3, target))
                return false;

            Phase3DirectCall call{};
            call.callPosition = position;
            call.target = target;
            call.targetFunction = FindVmFunctionByStart(functions, target);
            calls.push_back(call);
        }

        position += length;
    }

    return true;
}

static void LogPhase3FunctionOutline(
    Phase2ScrProgram* program,
    const VmFunctionRange& function,
    const std::vector<VmFunctionRange>& functions)
{
    std::vector<Phase3DirectCall> calls;
    if (!CollectDirectCalls(program, function, functions, calls))
    {
        Logf("[Phase3] SellHandler outline failed while collecting CALLs");
        return;
    }

    int nativeCount = 0;
    int networkGameNativeRefs = 0;

    for (uint32_t position = function.start; position < function.end;)
    {
        uint32_t length = 0;
        unsigned char* op = ScriptCodePointer(program, position);
        if (!op || !GetVmInstructionLength(program, position, length))
            break;

        if (*op == kVmNative)
        {
            uint8_t packed = 0;
            uint16_t index = 0;
            if (ReadVmNativeSignature(program, position, packed, index))
            {
                ++nativeCount;
                const unsigned int args =
                    static_cast<unsigned int>(packed >> 2);
                const unsigned int returns =
                    static_cast<unsigned int>(packed & 0x03);
                const bool networkGate =
                    g_phase2NetworkGameNativeIndex != 0xFFFF
                    && index == g_phase2NetworkGameNativeIndex;

                if (networkGate)
                    ++networkGameNativeRefs;

                Logf(
                    "[Phase3Native] func=%d site=0x%X nativeIndex=%u args=%u returns=%u networkGame=%s",
                    function.index,
                    position,
                    static_cast<unsigned int>(index),
                    args,
                    returns,
                    networkGate ? "yes" : "no");
            }
        }

        position += length;
    }

    Logf(
        "[Phase3] SellHandler outline func=%d calls=%u natives=%d networkGameNativeRefs=%d",
        function.index,
        static_cast<unsigned int>(calls.size()),
        nativeCount,
        networkGameNativeRefs);

    const size_t callLogLimit = calls.size() < 128 ? calls.size() : 128;
    for (size_t i = 0; i < callLogLimit; ++i)
    {
        const Phase3DirectCall& call = calls[i];
        int targetNetworkRefs = -1;

        if (call.targetFunction
            && g_phase2NetworkGameNativeIndex != 0xFFFF)
        {
            targetNetworkRefs = CountNativeIndexReferences(
                program,
                *call.targetFunction,
                g_phase2NetworkGameNativeIndex,
                nullptr);
        }

        Logf(
            "[Phase3Call] caller=%d site=0x%X target=0x%X targetFunc=%d args=%u size=%u networkGameRefs=%d",
            function.index,
            call.callPosition,
            call.target,
            call.targetFunction ? call.targetFunction->index : -1,
            call.targetFunction
                ? static_cast<unsigned int>(call.targetFunction->argCount)
                : 0U,
            call.targetFunction
                ? static_cast<unsigned int>(
                    call.targetFunction->end - call.targetFunction->start)
                : 0U,
            targetNetworkRefs);
    }

    if (calls.size() > callLogLimit)
    {
        Logf(
            "[Phase3] SellHandler CALL log truncated total=%u logged=%u",
            static_cast<unsigned int>(calls.size()),
            static_cast<unsigned int>(callLogLimit));
    }

    if (g_phase2NetworkGameNativeIndex == 0xFFFF)
        return;

    for (size_t i = 0; i < calls.size(); ++i)
    {
        if (!calls[i].targetFunction)
            continue;

        std::vector<uint32_t> positions;
        const int directRefs = CountNativeIndexReferences(
            program,
            *calls[i].targetFunction,
            g_phase2NetworkGameNativeIndex,
            &positions);

        if (directRefs > 0)
        {
            for (size_t j = 0; j < positions.size(); ++j)
            {
                Logf(
                    "[Phase3GateCandidate] depth=1 caller=%d call=0x%X targetFunc=%d gateNative=NETWORK_IS_GAME_IN_PROGRESS site=0x%X",
                    function.index,
                    calls[i].callPosition,
                    calls[i].targetFunction->index,
                    positions[j]);
            }
        }

        std::vector<Phase3DirectCall> secondLevelCalls;
        if (!CollectDirectCalls(
                program,
                *calls[i].targetFunction,
                functions,
                secondLevelCalls))
        {
            continue;
        }

        for (size_t j = 0; j < secondLevelCalls.size(); ++j)
        {
            if (!secondLevelCalls[j].targetFunction)
                continue;

            std::vector<uint32_t> secondPositions;
            const int secondRefs = CountNativeIndexReferences(
                program,
                *secondLevelCalls[j].targetFunction,
                g_phase2NetworkGameNativeIndex,
                &secondPositions);

            if (secondRefs <= 0)
                continue;

            for (size_t k = 0; k < secondPositions.size(); ++k)
            {
                Logf(
                    "[Phase3GateCandidate] depth=2 caller=%d viaFunc=%d call=0x%X targetFunc=%d gateNative=NETWORK_IS_GAME_IN_PROGRESS site=0x%X",
                    function.index,
                    calls[i].targetFunction->index,
                    secondLevelCalls[j].callPosition,
                    secondLevelCalls[j].targetFunction->index,
                    secondPositions[k]);
            }
        }
    }
}

static bool ResolvePhase3SellHandler(
    Phase2ScrProgram* program,
    const std::vector<VmFunctionRange>& functions,
    VmFunctionRange& sellHandler)
{
    static const char* kAnchorLabels[] =
    {
        "CMOD_SEL_T",
        "CMOD_SEL_0",
        "ITEM_COST",
        "CMOD_SEL_CONF",
        "CMOD_SEL",
        "CMOD_SOLD",
        "CMOD_NOSELL3",
        "CMOD_NOSELL5"
    };

    const size_t anchorCount =
        sizeof(kAnchorLabels) / sizeof(kAnchorLabels[0]);

    std::vector<Phase3SellAnchor> anchors(anchorCount);
    int availableAnchors = 0;

    for (size_t i = 0; i < anchorCount; ++i)
    {
        anchors[i].label = kAnchorLabels[i];
        FindExactScriptStringOffsets(
            program,
            kAnchorLabels[i],
            anchors[i].offsets);

        if (!anchors[i].offsets.empty())
            ++availableAnchors;

        Logf(
            "[Phase3] StringAnchor label=%s matches=%u firstOffset=0x%X",
            kAnchorLabels[i],
            static_cast<unsigned int>(anchors[i].offsets.size()),
            anchors[i].offsets.empty() ? 0U : anchors[i].offsets[0]);
    }

    if (availableAnchors < 3)
    {
        Logf(
            "[Phase3] SellHandler resolution failed safely: only %d Sell string anchors found",
            availableAnchors);
        return false;
    }

    int bestDistinct = 0;
    int bestFunctionCount = 0;
    VmFunctionRange best{};
    std::vector<Phase3StringReference> bestReferences;

    for (size_t i = 0; i < functions.size(); ++i)
    {
        std::vector<Phase3StringReference> references;
        int distinct = 0;

        if (!CollectSellStringReferences(
                program,
                functions[i],
                anchors,
                references,
                distinct))
        {
            Logf(
                "[Phase3] SellHandler resolution failed safely while scanning func=%d",
                functions[i].index);
            return false;
        }

        if (distinct >= 2)
        {
            Logf(
                "[Phase3] SellHandlerCandidate func=%d start=0x%X end=0x%X args=%u distinctAnchors=%d totalRefs=%u",
                functions[i].index,
                functions[i].start,
                functions[i].end,
                static_cast<unsigned int>(functions[i].argCount),
                distinct,
                static_cast<unsigned int>(references.size()));
        }

        if (distinct > bestDistinct)
        {
            bestDistinct = distinct;
            bestFunctionCount = 1;
            best = functions[i];
            bestReferences = references;
        }
        else if (distinct == bestDistinct && distinct > 0)
        {
            ++bestFunctionCount;
        }
    }

    if (bestDistinct < 3 || bestFunctionCount != 1)
    {
        Logf(
            "[Phase3] SellHandler resolution failed safely: bestDistinct=%d tiedFunctions=%d",
            bestDistinct,
            bestFunctionCount);
        return false;
    }

    sellHandler = best;

    Logf(
        "[Phase3] SellHandler resolved func=%d start=0x%X end=0x%X args=%u size=%u distinctAnchors=%d",
        best.index,
        best.start,
        best.end,
        static_cast<unsigned int>(best.argCount),
        static_cast<unsigned int>(best.end - best.start),
        bestDistinct);

    for (size_t i = 0; i < bestReferences.size(); ++i)
    {
        const Phase3StringReference& reference = bestReferences[i];
        Logf(
            "[Phase3StringRef] func=%d label=%s push=0x%X stringOp=0x%X stringOffset=0x%X",
            best.index,
            anchors[reference.anchorIndex].label,
            reference.pushPosition,
            reference.stringPosition,
            reference.stringOffset);
    }

    return true;
}

static bool ResolvePhase3SellCooldownPath(
    Phase2ScrProgram* program,
    const VmFunctionRange& sellHandler)
{
    g_phase3SellCooldownPath =
        Phase3SellCooldownPath{};

    if (!program
        || sellHandler.end <= sellHandler.start)
    {
        return false;
    }

    std::vector<uint32_t> noSell3Offsets;
    FindExactScriptStringOffsets(
        program,
        "CMOD_NOSELL3",
        noSell3Offsets);

    if (noSell3Offsets.empty())
    {
        Logf(
            "[Gameplay] SellCooldownPath=no reason=CMOD_NOSELL3 string not found");
        return false;
    }

    Phase3SellAnchor anchor{};
    anchor.label = "CMOD_NOSELL3";
    anchor.offsets = noSell3Offsets;

    std::vector<Phase3SellAnchor> anchors;
    anchors.push_back(anchor);

    std::vector<Phase3StringReference> references;
    int distinct = 0;
    if (!CollectSellStringReferences(
            program,
            sellHandler,
            anchors,
            references,
            distinct)
        || references.empty())
    {
        Logf(
            "[Gameplay] SellCooldownPath=no reason=CMOD_NOSELL3 reference not found in Sell handler");
        return false;
    }

    int matches = 0;
    uint32_t matchedMessagePush = 0;
    uint32_t matchedNativeSite = 0;
    uint16_t matchedNativeIndex = 0xFFFF;

    for (size_t r = 0; r < references.size(); ++r)
    {
        const uint32_t messagePush =
            references[r].pushPosition;

        const uint32_t searchStart =
            messagePush > sellHandler.start + 0x100U
                ? messagePush - 0x100U
                : sellHandler.start;

        for (uint32_t position = searchStart;
             position < messagePush;)
        {
            uint32_t length = 0;
            unsigned char* op =
                ScriptCodePointer(
                    program,
                    position);

            if (!op
                || !GetVmInstructionLength(
                    program,
                    position,
                    length))
            {
                return false;
            }

            if (*op == kVmNative)
            {
                uint8_t packed = 0;
                uint16_t nativeIndex = 0;

                if (!ReadVmNativeSignature(
                        program,
                        position,
                        packed,
                        nativeIndex))
                {
                    return false;
                }

                if ((packed >> 2) == 0
                    && (packed & 0x03) == 1)
                {
                    uint32_t p =
                        position + length;

                    uint32_t nextLength = 0;
                    unsigned char* pushStat =
                        ScriptCodePointer(
                            program,
                            p);

                    if (pushStat
                        && *pushStat
                            == kVmPushConstU32
                        && GetVmInstructionLength(
                            program,
                            p,
                            nextLength))
                    {
                        p += nextLength;

                        unsigned char* getterCall =
                            ScriptCodePointer(
                                program,
                                p);

                        if (getterCall
                            && *getterCall
                                == kVmCall
                            && GetVmInstructionLength(
                                program,
                                p,
                                nextLength))
                        {
                            p += nextLength;

                            unsigned char* subtract =
                                ScriptCodePointer(
                                    program,
                                    p);

                            if (subtract
                                && *subtract == 0x02
                                && GetVmInstructionLength(
                                    program,
                                    p,
                                    nextLength))
                            {
                                p += nextLength;

                                int thresholdBase = 0;
                                if (TryGetVmPushedInt(
                                        program,
                                        p,
                                        thresholdBase)
                                    && thresholdBase
                                        == 2880)
                                {
                                    bool hasLessThan =
                                        false;
                                    uint32_t verify =
                                        p;

                                    while (verify
                                        < messagePush
                                        && verify
                                            < p + 0x30U)
                                    {
                                        uint32_t verifyLength = 0;
                                        unsigned char* verifyOp =
                                            ScriptCodePointer(
                                                program,
                                                verify);

                                        if (!verifyOp
                                            || !GetVmInstructionLength(
                                                program,
                                                verify,
                                                verifyLength))
                                        {
                                            return false;
                                        }

                                        if (*verifyOp == 0x0C)
                                        {
                                            hasLessThan = true;
                                            break;
                                        }

                                        verify +=
                                            verifyLength;
                                    }

                                    if (hasLessThan)
                                    {
                                        ++matches;
                                        matchedMessagePush =
                                            messagePush;
                                        matchedNativeSite =
                                            position;
                                        matchedNativeIndex =
                                            nativeIndex;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            position += length;
        }
    }

    if (matches != 1
        || matchedNativeSite == 0
        || matchedNativeIndex == 0xFFFF)
    {
        Logf(
            "[Gameplay] SellCooldownPath=no reason=clock gate not unique matches=%d",
            matches);
        return false;
    }

    g_phase3SellCooldownPath.resolved =
        true;
    g_phase3SellCooldownPath.messagePush =
        matchedMessagePush;
    g_phase3SellCooldownPath.clockNativeSite =
        matchedNativeSite;
    g_phase3SellCooldownPath.clockNativeIndex =
        matchedNativeIndex;

    Logf(
        "[Gameplay] SellCooldownPath=yes message=CMOD_NOSELL3 push=0x%X clockSite=0x%X nativeIndex=%u thresholdBase=2880",
        matchedMessagePush,
        matchedNativeSite,
        static_cast<unsigned int>(
            matchedNativeIndex));

    return true;
}

static const char* GetVmOpcodeName(unsigned char op)
{
    switch (op)
    {
    case 0x00: return "NOP";
    case 0x06: return "INOT";
    case 0x08: return "IEQ";
    case 0x09: return "INE";
    case 0x0A: return "IGT";
    case 0x0B: return "IGE";
    case 0x0C: return "ILT";
    case 0x0D: return "ILE";
    case 0x1F: return "IAND";
    case 0x20: return "IOR";
    case 0x25: return "PUSH_CONST_U8";
    case 0x26: return "PUSH_CONST_U8_U8";
    case 0x27: return "PUSH_CONST_U8_U8_U8";
    case 0x28: return "PUSH_CONST_U32";
    case 0x2A: return "DUP";
    case 0x2B: return "DROP";
    case 0x2C: return "NATIVE";
    case 0x2D: return "ENTER";
    case 0x2E: return "LEAVE";
    case 0x2F: return "LOAD";
    case 0x30: return "STORE";
    case 0x31: return "STORE_REV";
    case 0x34: return "ARRAY_U8";
    case 0x35: return "ARRAY_U8_LOAD";
    case 0x36: return "ARRAY_U8_STORE";
    case 0x37: return "LOCAL_U8";
    case 0x38: return "LOCAL_U8_LOAD";
    case 0x39: return "LOCAL_U8_STORE";
    case 0x40: return "IOFFSET_U8";
    case 0x41: return "IOFFSET_U8_LOAD";
    case 0x42: return "IOFFSET_U8_STORE";
    case 0x43: return "PUSH_CONST_S16";
    case 0x46: return "IOFFSET_S16";
    case 0x47: return "IOFFSET_S16_LOAD";
    case 0x48: return "IOFFSET_S16_STORE";
    case 0x4C: return "LOCAL_U16";
    case 0x4D: return "LOCAL_U16_LOAD";
    case 0x4E: return "LOCAL_U16_STORE";
    case 0x4F: return "STATIC_U16";
    case 0x50: return "STATIC_U16_LOAD";
    case 0x51: return "STATIC_U16_STORE";
    case 0x52: return "GLOBAL_U16";
    case 0x53: return "GLOBAL_U16_LOAD";
    case 0x54: return "GLOBAL_U16_STORE";
    case 0x55: return "J";
    case 0x56: return "JZ";
    case 0x57: return "IEQ_JZ";
    case 0x58: return "INE_JZ";
    case 0x59: return "IGT_JZ";
    case 0x5A: return "IGE_JZ";
    case 0x5B: return "ILT_JZ";
    case 0x5C: return "ILE_JZ";
    case 0x5D: return "CALL";
    case 0x5E: return "STATIC_U24";
    case 0x5F: return "STATIC_U24_LOAD";
    case 0x60: return "STATIC_U24_STORE";
    case 0x61: return "GLOBAL_U24";
    case 0x62: return "GLOBAL_U24_LOAD";
    case 0x63: return "GLOBAL_U24_STORE";
    case 0x64: return "PUSH_CONST_U24";
    case 0x65: return "SWITCH";
    case 0x66: return "STRING";
    case 0x6D: return "CATCH";
    case 0x6E: return "THROW";
    case 0x6F: return "CALLINDIRECT";
    case 0x70: return "PUSH_CONST_M1";
    case 0x71: return "PUSH_CONST_0";
    case 0x72: return "PUSH_CONST_1";
    case 0x73: return "PUSH_CONST_2";
    case 0x74: return "PUSH_CONST_3";
    case 0x75: return "PUSH_CONST_4";
    case 0x76: return "PUSH_CONST_5";
    case 0x77: return "PUSH_CONST_6";
    case 0x78: return "PUSH_CONST_7";
    case 0x82: return "IBITTEST";
    default: return "OP";
    }
}

static bool ReadScriptSigned16(
    Phase2ScrProgram* program,
    uint32_t position,
    int16_t& value)
{
    uint32_t raw = 0;
    if (!ReadScriptUnsigned(program, position, 2, raw))
        return false;

    value = static_cast<int16_t>(raw & 0xFFFFU);
    return true;
}

static bool ResolvePhase3SellEligibilityFunction(
    Phase2ScrProgram* program,
    const std::vector<VmFunctionRange>& functions,
    VmFunctionRange& eligibilityFunction,
    uint32_t& noSell1MessagePush)
{
    eligibilityFunction = VmFunctionRange{};
    noSell1MessagePush = 0;

    std::vector<uint32_t> noSell1Offsets;
    FindExactScriptStringOffsets(
        program,
        "CMOD_NOSELL1",
        noSell1Offsets);

    if (noSell1Offsets.empty())
    {
        Logf(
            "[Phase3M] SellEligibility=no reason=CMOD_NOSELL1 string not found");
        return false;
    }

    Phase3SellAnchor anchor{};
    anchor.label = "CMOD_NOSELL1";
    anchor.offsets = noSell1Offsets;

    std::vector<Phase3SellAnchor> anchors;
    anchors.push_back(anchor);

    int totalReferences = 0;
    int candidateFunctions = 0;
    VmFunctionRange candidate{};
    uint32_t candidatePush = 0;

    for (size_t i = 0; i < functions.size(); ++i)
    {
        std::vector<Phase3StringReference> references;
        int distinct = 0;

        if (!CollectSellStringReferences(
                program,
                functions[i],
                anchors,
                references,
                distinct))
        {
            Logf(
                "[Phase3M] SellEligibility=no reason=parser failure func=%d",
                functions[i].index);
            return false;
        }

        if (references.empty())
            continue;

        totalReferences += static_cast<int>(references.size());
        ++candidateFunctions;

        Logf(
            "[Phase3M] SellEligibilityCandidate func=%d start=0x%X end=0x%X refs=%u",
            functions[i].index,
            functions[i].start,
            functions[i].end,
            static_cast<unsigned int>(references.size()));

        if (references.size() == 1)
        {
            candidate = functions[i];
            candidatePush = references[0].pushPosition;
        }
    }

    if (candidateFunctions != 1
        || totalReferences != 1
        || !candidate.found
        || candidatePush == 0)
    {
        Logf(
            "[Phase3M] SellEligibility=no reason=CMOD_NOSELL1 reference not unique functions=%d references=%d",
            candidateFunctions,
            totalReferences);
        return false;
    }

    eligibilityFunction = candidate;
    noSell1MessagePush = candidatePush;

    Logf(
        "[Phase3M] SellEligibility=yes func=%d start=0x%X end=0x%X noSell1=0x%X",
        eligibilityFunction.index,
        eligibilityFunction.start,
        eligibilityFunction.end,
        noSell1MessagePush);

    return true;
}

static bool ApplyHighValueSellRestrictionPatch(
    Phase2ScrProgram* program,
    const VmFunctionRange& eligibilityFunction,
    uint32_t messagePush)
{
    if (!program || !eligibilityFunction.found || messagePush == 0)
        return false;

    if (g_highValueSellPatchApplied
        && g_highValueSellPatchedProgram == program)
    {
        return true;
    }

    uint32_t precedingInstruction = 0xFFFFFFFFU;

    for (uint32_t position = eligibilityFunction.start;
         position < messagePush;)
    {
        uint32_t length = 0;
        unsigned char* op = ScriptCodePointer(program, position);

        if (!op || !GetVmInstructionLength(program, position, length))
        {
            Logf(
                "[Phase3M] HighValueSellBypass=no reason=parser failure pc=0x%X",
                position);
            return false;
        }

        if (position + length > messagePush)
        {
            Logf(
                "[Phase3M] HighValueSellBypass=no reason=instruction overlap message=0x%X pc=0x%X",
                messagePush,
                position);
            return false;
        }

        precedingInstruction = position;
        position += length;
    }

    unsigned char* pushOp =
        ScriptCodePointer(program, messagePush);
    unsigned char* branchOp =
        precedingInstruction != 0xFFFFFFFFU
            ? ScriptCodePointer(program, precedingInstruction)
            : nullptr;

    uint32_t precedingLength = 0;
    if (!pushOp
        || !branchOp
        || !GetVmInstructionLength(
            program,
            precedingInstruction,
            precedingLength)
        || precedingInstruction + precedingLength != messagePush
        || *branchOp != kVmJz
        || *pushOp != kVmPushConstU24)
    {
        Logf(
            "[Phase3M] HighValueSellBypass=no reason=unexpected CMOD_NOSELL1 branch shape branch=0x%X message=0x%X branchOp=0x%02X pushOp=0x%02X",
            precedingInstruction,
            messagePush,
            branchOp ? static_cast<unsigned int>(*branchOp) : 0U,
            pushOp ? static_cast<unsigned int>(*pushOp) : 0U);
        return false;
    }

    int16_t branchRelative = 0;
    if (!ReadScriptSigned16(
            program,
            precedingInstruction + 1,
            branchRelative))
    {
        Logf(
            "[Phase3M] HighValueSellBypass=no reason=failed reading CMOD_NOSELL1 skip branch");
        return false;
    }

    const int64_t skipTarget64 =
        static_cast<int64_t>(precedingInstruction)
        + static_cast<int64_t>(precedingLength)
        + static_cast<int64_t>(branchRelative);

    if (skipTarget64 <= static_cast<int64_t>(messagePush)
        || skipTarget64 > static_cast<int64_t>(eligibilityFunction.end))
    {
        Logf(
            "[Phase3M] HighValueSellBypass=no reason=invalid CMOD_NOSELL1 skip target branch=0x%X message=0x%X target=0x%llX",
            precedingInstruction,
            messagePush,
            static_cast<unsigned long long>(skipTarget64));
        return false;
    }

    const uint32_t skipTarget =
        static_cast<uint32_t>(skipTarget64);

    unsigned char* stringOp =
        ScriptCodePointer(program, messagePush + 4);

    if (!stringOp || *stringOp != kVmString)
    {
        Logf(
            "[Phase3M] HighValueSellBypass=no reason=CMOD_NOSELL1 STRING opcode missing message=0x%X",
            messagePush);
        return false;
    }

    bool sawCall = false;
    bool sawExitJump = false;

    for (uint32_t position = messagePush + 5;
         position < skipTarget;)
    {
        uint32_t length = 0;
        unsigned char* op = ScriptCodePointer(program, position);

        if (!op || !GetVmInstructionLength(program, position, length))
            return false;

        if (*op == kVmCall)
            sawCall = true;
        else if (*op == kVmJ)
            sawExitJump = true;

        position += length;
    }

    if (!sawCall || !sawExitJump)
    {
        Logf(
            "[Phase3M] HighValueSellBypass=no reason=CMOD_NOSELL1 block validation failed call=%s exitJump=%s message=0x%X target=0x%X",
            sawCall ? "yes" : "no",
            sawExitJump ? "yes" : "no",
            messagePush,
            skipTarget);
        return false;
    }

    const int64_t newRelative64 =
        static_cast<int64_t>(skipTarget)
        - static_cast<int64_t>(messagePush + 3);

    if (newRelative64 < -32768 || newRelative64 > 32767)
    {
        Logf(
            "[Phase3M] HighValueSellBypass=no reason=replacement jump out of range message=0x%X target=0x%X",
            messagePush,
            skipTarget);
        return false;
    }

    const int16_t newRelative =
        static_cast<int16_t>(newRelative64);

    const unsigned char original[4] =
    {
        pushOp[0], pushOp[1], pushOp[2], pushOp[3]
    };

    const unsigned char patch[4] =
    {
        kVmJ,
        static_cast<unsigned char>(
            static_cast<uint16_t>(newRelative) & 0xFFU),
        static_cast<unsigned char>(
            (static_cast<uint16_t>(newRelative) >> 8) & 0xFFU),
        kVmNop
    };

    if (!WriteVmPatch(program, messagePush, patch))
    {
        Logf(
            "[Phase3M] HighValueSellBypass=no reason=patch write/verify failed message=0x%X",
            messagePush);
        return false;
    }

    g_highValueSellPatchedProgram = program;
    g_highValueSellPatchPosition = messagePush;
    std::memcpy(
        g_highValueSellOriginal,
        original,
        sizeof(g_highValueSellOriginal));
    g_highValueSellPatchApplied = true;

    Logf(
        "[Phase3M] HighValueSellBypass=yes label=CMOD_NOSELL1 eligibilityFunc=%d message=0x%X branch=0x%X skipTarget=0x%X original=%02X %02X %02X %02X patch=%02X %02X %02X %02X",
        eligibilityFunction.index,
        messagePush,
        precedingInstruction,
        skipTarget,
        original[0], original[1], original[2], original[3],
        patch[0], patch[1], patch[2], patch[3]);

    Logf(
        "[Phase3M] Scope: only the CMOD_NOSELL1 high-value rejection block is bypassed. Other Sell rejection messages are not directly bypassed.");

    return true;
}

static bool ResolvePhase3PlayerOwnedHelper(
    Phase2ScrProgram* program,
    const std::vector<VmFunctionRange>& functions,
    VmFunctionRange& playerOwnedHelper)
{
    playerOwnedHelper = VmFunctionRange{};

    std::vector<uint32_t> playerVehicleOffsets;
    FindExactScriptStringOffsets(
        program,
        "Player_Vehicle",
        playerVehicleOffsets);

    if (playerVehicleOffsets.empty())
    {
        Logf(
            "[Phase3M] PlayerOwnedHelper=no reason=Player_Vehicle string not found");
        return false;
    }

    Phase3SellAnchor anchor{};
    anchor.label = "Player_Vehicle";
    anchor.offsets = playerVehicleOffsets;

    std::vector<Phase3SellAnchor> anchors;
    anchors.push_back(anchor);

    int qualifiedCandidates = 0;

    for (size_t i = 0; i < functions.size(); ++i)
    {
        if (functions[i].argCount != 1)
            continue;

        std::vector<Phase3StringReference> references;
        int distinct = 0;

        if (!CollectSellStringReferences(
                program,
                functions[i],
                anchors,
                references,
                distinct))
        {
            return false;
        }

        if (references.size() < 3)
            continue;

        const int networkRefs =
            CountNativeIndexReferences(
                program,
                functions[i],
                g_phase2NetworkGameNativeIndex,
                nullptr);

        if (networkRefs <= 0)
            continue;

        ++qualifiedCandidates;
        playerOwnedHelper = functions[i];

        Logf(
            "[Phase3M] PlayerOwnedHelperCandidate func=%d start=0x%X end=0x%X args=%u playerVehicleRefs=%u networkGameRefs=%d",
            functions[i].index,
            functions[i].start,
            functions[i].end,
            static_cast<unsigned int>(functions[i].argCount),
            static_cast<unsigned int>(references.size()),
            networkRefs);
    }

    if (qualifiedCandidates != 1 || !playerOwnedHelper.found)
    {
        Logf(
            "[Phase3M] PlayerOwnedHelper=no reason=structural target not unique candidates=%d",
            qualifiedCandidates);
        playerOwnedHelper = VmFunctionRange{};
        return false;
    }

    Logf(
        "[Phase3M] PlayerOwnedHelper=yes func=%d start=0x%X end=0x%X args=%u",
        playerOwnedHelper.index,
        playerOwnedHelper.start,
        playerOwnedHelper.end,
        static_cast<unsigned int>(playerOwnedHelper.argCount));

    return true;
}

static bool CollectCallsToVmFunction(
    Phase2ScrProgram* program,
    const VmFunctionRange& caller,
    uint32_t targetStart,
    std::vector<uint32_t>& calls)
{
    calls.clear();

    for (uint32_t position = caller.start;
         position < caller.end;)
    {
        uint32_t length = 0;
        unsigned char* op = ScriptCodePointer(program, position);

        if (!op || !GetVmInstructionLength(program, position, length))
            return false;

        if (*op == kVmCall)
        {
            uint32_t target = 0;
            if (!ReadScriptUnsigned(program, position + 1, 3, target))
                return false;

            if (target == targetStart)
                calls.push_back(position);
        }

        position += length;
    }

    return true;
}

static bool ApplySellPlayerOwnedCallPatches(
    Phase2ScrProgram* program,
    const VmFunctionRange& eligibilityFunction,
    uint32_t noSell1MessagePush,
    const VmFunctionRange& sellHandler,
    const VmFunctionRange& playerOwnedHelper)
{
    if (!program
        || !eligibilityFunction.found
        || !sellHandler.found
        || !playerOwnedHelper.found
        || playerOwnedHelper.argCount != 1)
    {
        return false;
    }

    if (g_sellOwnershipPatchApplied
        && g_sellOwnershipPatchedProgram == program)
    {
        return true;
    }

    std::vector<uint32_t> eligibilityCalls;
    std::vector<uint32_t> sellHandlerCalls;

    if (!CollectCallsToVmFunction(
            program,
            eligibilityFunction,
            playerOwnedHelper.start,
            eligibilityCalls)
        || !CollectCallsToVmFunction(
            program,
            sellHandler,
            playerOwnedHelper.start,
            sellHandlerCalls))
    {
        Logf(
            "[Phase3M] PlayerOwnedBypass=no reason=CALL collection failed");
        return false;
    }

    if (eligibilityCalls.empty())
    {
        Logf(
            "[Phase3M] PlayerOwnedBypass=no reason=no ownership calls in Sell eligibility function");
        return false;
    }

    size_t nearestIndex = 0;
    uint64_t nearestDistance = ~static_cast<uint64_t>(0);

    for (size_t i = 0; i < eligibilityCalls.size(); ++i)
    {
        const uint64_t distance =
            eligibilityCalls[i] > noSell1MessagePush
                ? static_cast<uint64_t>(
                    eligibilityCalls[i] - noSell1MessagePush)
                : static_cast<uint64_t>(
                    noSell1MessagePush - eligibilityCalls[i]);

        if (distance < nearestDistance)
        {
            nearestDistance = distance;
            nearestIndex = i;
        }
    }

    size_t clusterFirst = nearestIndex;
    size_t clusterLast = nearestIndex;
    static const uint32_t kOwnershipClusterGap = 0x300;

    while (clusterFirst > 0
        && eligibilityCalls[clusterFirst]
            - eligibilityCalls[clusterFirst - 1]
            <= kOwnershipClusterGap)
    {
        --clusterFirst;
    }

    while (clusterLast + 1 < eligibilityCalls.size()
        && eligibilityCalls[clusterLast + 1]
            - eligibilityCalls[clusterLast]
            <= kOwnershipClusterGap)
    {
        ++clusterLast;
    }

    const size_t eligibilityClusterCount =
        clusterLast - clusterFirst + 1;

    if (eligibilityClusterCount < 3
        || nearestDistance > 0x600)
    {
        Logf(
            "[Phase3M] PlayerOwnedBypass=no reason=Sell ownership call cluster validation failed totalCalls=%u clusterCalls=%u nearestNoSell1=0x%llX",
            static_cast<unsigned int>(eligibilityCalls.size()),
            static_cast<unsigned int>(eligibilityClusterCount),
            static_cast<unsigned long long>(nearestDistance));
        return false;
    }

    std::vector<uint32_t> patchPositions;

    for (size_t i = clusterFirst; i <= clusterLast; ++i)
        patchPositions.push_back(eligibilityCalls[i]);

    for (size_t i = 0; i < sellHandlerCalls.size(); ++i)
    {
        bool duplicate = false;
        for (size_t j = 0; j < patchPositions.size(); ++j)
        {
            if (patchPositions[j] == sellHandlerCalls[i])
            {
                duplicate = true;
                break;
            }
        }

        if (!duplicate)
            patchPositions.push_back(sellHandlerCalls[i]);
    }

    if (patchPositions.empty() || patchPositions.size() > 24)
    {
        Logf(
            "[Phase3M] PlayerOwnedBypass=no reason=unexpected patch site count=%u",
            static_cast<unsigned int>(patchPositions.size()));
        return false;
    }

    struct PreparedPatch
    {
        uint32_t position;
        unsigned char original[4];
    };

    std::vector<PreparedPatch> prepared;
    prepared.reserve(patchPositions.size());

    for (size_t i = 0; i < patchPositions.size(); ++i)
    {
        const uint32_t position = patchPositions[i];
        unsigned char* op = ScriptCodePointer(program, position);
        uint32_t length = 0;
        uint32_t target = 0;

        if (!op
            || !GetVmInstructionLength(program, position, length)
            || length != 4
            || *op != kVmCall
            || !ReadScriptUnsigned(program, position + 1, 3, target)
            || target != playerOwnedHelper.start)
        {
            Logf(
                "[Phase3M] PlayerOwnedBypass=no reason=site validation failed site=0x%X",
                position);
            return false;
        }

        PreparedPatch entry{};
        entry.position = position;
        entry.original[0] = op[0];
        entry.original[1] = op[1];
        entry.original[2] = op[2];
        entry.original[3] = op[3];
        prepared.push_back(entry);
    }

    const unsigned char patch[4] =
    {
        kVmDrop,
        kVmPushConst1,
        kVmNop,
        kVmNop
    };

    size_t written = 0;
    for (; written < prepared.size(); ++written)
    {
        if (!WriteVmPatch(
                program,
                prepared[written].position,
                patch))
        {
            break;
        }
    }

    if (written != prepared.size())
    {
        for (size_t i = 0; i < written; ++i)
        {
            WriteVmPatch(
                program,
                prepared[i].position,
                prepared[i].original);
        }

        Logf(
            "[Phase3M] PlayerOwnedBypass=no reason=patch write failed siteIndex=%u rolledBack=%u",
            static_cast<unsigned int>(written),
            static_cast<unsigned int>(written));
        return false;
    }

    g_sellOwnershipPatchedProgram = program;
    g_sellOwnershipPatchPositions = patchPositions;
    g_sellOwnershipPatchBackups.clear();
    g_sellOwnershipPatchBackups.reserve(
        prepared.size());

    for (size_t i = 0;
         i < prepared.size();
         ++i)
    {
        VmPatchBackup backup{};
        backup.position =
            prepared[i].position;
        std::memcpy(
            backup.original,
            prepared[i].original,
            sizeof(backup.original));
        g_sellOwnershipPatchBackups.push_back(
            backup);
    }

    g_sellOwnershipPatchApplied = true;

    Logf(
        "[Phase3M] PlayerOwnedBypass=yes helperFunc=%d helperStart=0x%X eligibilityFunc=%d eligibilityCallsTotal=%u eligibilityClusterPatched=%u sellHandlerFunc=%d sellHandlerCallsPatched=%u totalPatched=%u patch=DROP,PUSH_CONST_1,NOP,NOP",
        playerOwnedHelper.index,
        playerOwnedHelper.start,
        eligibilityFunction.index,
        static_cast<unsigned int>(eligibilityCalls.size()),
        static_cast<unsigned int>(eligibilityClusterCount),
        sellHandler.index,
        static_cast<unsigned int>(sellHandlerCalls.size()),
        static_cast<unsigned int>(patchPositions.size()));

    for (size_t i = 0; i < patchPositions.size(); ++i)
    {
        Logf(
            "[Phase3M] PlayerOwnedCallPatched ordinal=%u site=0x%X",
            static_cast<unsigned int>(i),
            patchPositions[i]);
    }

    Logf(
        "[Phase3M] Scope: only carmod_shop CALL sites to the structurally resolved Player_Vehicle ownership helper inside the Sell handler and the Sell eligibility call cluster are forced true. The ownership helper itself is not modified globally.");

    return true;
}

static void LogPhase3FocusedInstruction(
    Phase2ScrProgram* program,
    uint32_t position,
    uint32_t length,
    const std::vector<VmFunctionRange>& functions)
{
    unsigned char* opPtr = ScriptCodePointer(program, position);
    if (!opPtr)
        return;

    const unsigned char op = *opPtr;
    const char* name = GetVmOpcodeName(op);

    if (op == kVmCall)
    {
        uint32_t target = 0;
        if (ReadScriptUnsigned(program, position + 1, 3, target))
        {
            const VmFunctionRange* targetFunction =
                FindVmFunctionByStart(functions, target);
            Logf(
                "[Phase3Insn] pc=0x%X op=%s target=0x%X targetFunc=%d args=%u",
                position,
                name,
                target,
                targetFunction ? targetFunction->index : -1,
                targetFunction
                    ? static_cast<unsigned int>(targetFunction->argCount)
                    : 0U);
            return;
        }
    }

    if (op == kVmNative)
    {
        uint8_t packed = 0;
        uint16_t nativeIndex = 0;
        if (ReadVmNativeSignature(program, position, packed, nativeIndex))
        {
            Logf(
                "[Phase3Insn] pc=0x%X op=%s nativeIndex=%u args=%u returns=%u networkGame=%s",
                position,
                name,
                static_cast<unsigned int>(nativeIndex),
                static_cast<unsigned int>(packed >> 2),
                static_cast<unsigned int>(packed & 0x03),
                nativeIndex == g_phase2NetworkGameNativeIndex ? "yes" : "no");
            return;
        }
    }

    if (op >= 0x55 && op <= 0x5C)
    {
        int16_t relative = 0;
        if (ReadScriptSigned16(program, position + 1, relative))
        {
            const uint32_t target = static_cast<uint32_t>(
                static_cast<int64_t>(position)
                + 3
                + static_cast<int64_t>(relative));
            Logf(
                "[Phase3Insn] pc=0x%X op=%s rel=%d target=0x%X",
                position,
                name,
                static_cast<int>(relative),
                target);
            return;
        }
    }

    int pushedValue = 0;
    if (TryGetVmPushedInt(program, position, pushedValue))
    {
        Logf(
            "[Phase3Insn] pc=0x%X op=%s value=%d",
            position,
            name,
            pushedValue);
        return;
    }

    if (op == kVmLocalU8
        || op == kVmLocalU8Load
        || op == 0x39)
    {
        uint32_t value = 0;
        if (ReadScriptUnsigned(program, position + 1, 1, value))
        {
            Logf(
                "[Phase3Insn] pc=0x%X op=%s index=%u",
                position,
                name,
                value);
            return;
        }
    }

    if (op == kVmLocalU16
        || op == kVmLocalU16Load
        || op == 0x4E
        || op == 0x52
        || op == 0x53
        || op == 0x54)
    {
        uint32_t value = 0;
        if (ReadScriptUnsigned(program, position + 1, 2, value))
        {
            Logf(
                "[Phase3Insn] pc=0x%X op=%s index=%u",
                position,
                name,
                value);
            return;
        }
    }

    if ((op >= 0x5E && op <= 0x64) && op != kVmCall)
    {
        uint32_t value = 0;
        if (ReadScriptUnsigned(program, position + 1, 3, value))
        {
            Logf(
                "[Phase3Insn] pc=0x%X op=%s value=0x%X",
                position,
                name,
                value);
            return;
        }
    }

    if (op == kVmSwitch)
    {
        unsigned char* count = ScriptCodePointer(program, position + 1);
        Logf(
            "[Phase3Insn] pc=0x%X op=%s cases=%u length=%u",
            position,
            name,
            count ? static_cast<unsigned int>(*count) : 0U,
            length);
        return;
    }

    unsigned int raw0 = 0;
    unsigned int raw1 = 0;
    unsigned int raw2 = 0;
    unsigned int raw3 = 0;
    unsigned char* b0 = ScriptCodePointer(program, position);
    unsigned char* b1 = length > 1 ? ScriptCodePointer(program, position + 1) : nullptr;
    unsigned char* b2 = length > 2 ? ScriptCodePointer(program, position + 2) : nullptr;
    unsigned char* b3 = length > 3 ? ScriptCodePointer(program, position + 3) : nullptr;
    raw0 = b0 ? *b0 : 0U;
    raw1 = b1 ? *b1 : 0U;
    raw2 = b2 ? *b2 : 0U;
    raw3 = b3 ? *b3 : 0U;

    Logf(
        "[Phase3Insn] pc=0x%X op=%s length=%u raw=%02X %02X %02X %02X",
        position,
        name,
        length,
        raw0,
        raw1,
        raw2,
        raw3);
}

static void LogPhase3FocusedSellBytecode(
    Phase2ScrProgram* program,
    const VmFunctionRange& sellHandler,
    const std::vector<VmFunctionRange>& functions)
{
    std::vector<uint32_t> itemCostOffsets;
    std::vector<uint32_t> confirmOffsets;
    std::vector<uint32_t> sellOffsets;
    FindExactScriptStringOffsets(program, "ITEM_COST", itemCostOffsets);
    FindExactScriptStringOffsets(program, "CMOD_SEL_CONF", confirmOffsets);
    FindExactScriptStringOffsets(program, "CMOD_SEL", sellOffsets);

    uint32_t itemCostPush = 0;
    uint32_t confirmPush = 0;
    uint32_t finalSellPush = 0;

    for (uint32_t position = sellHandler.start;
         position < sellHandler.end;)
    {
        uint32_t length = 0;
        unsigned char* op = ScriptCodePointer(program, position);
        if (!op || !GetVmInstructionLength(program, position, length))
            return;

        int pushedValue = -1;
        if (TryGetVmPushedInt(program, position, pushedValue)
            && pushedValue >= 0)
        {
            unsigned char* next =
                ScriptCodePointer(program, position + length);
            if (next && *next == kVmString)
            {
                const uint32_t value = static_cast<uint32_t>(pushedValue);

                for (size_t i = 0; i < itemCostOffsets.size(); ++i)
                {
                    if (itemCostOffsets[i] == value && itemCostPush == 0)
                        itemCostPush = position;
                }

                for (size_t i = 0; i < confirmOffsets.size(); ++i)
                {
                    if (confirmOffsets[i] == value && confirmPush == 0)
                        confirmPush = position;
                }

                for (size_t i = 0; i < sellOffsets.size(); ++i)
                {
                    if (sellOffsets[i] == value
                        && position > confirmPush
                        && position > finalSellPush)
                    {
                        finalSellPush = position;
                    }
                }
            }
        }

        position += length;
    }

    if (confirmPush == 0)
    {
        Logf("[Phase3Focus] skipped reason=CMOD_SEL_CONF code reference not found");
        return;
    }

    uint32_t requestedStart = itemCostPush != 0 && itemCostPush > 0x60
        ? itemCostPush - 0x60
        : (confirmPush > 0x90 ? confirmPush - 0x90 : sellHandler.start);
    if (requestedStart < sellHandler.start)
        requestedStart = sellHandler.start;

    const uint32_t requestedEnd = sellHandler.end;

    Logf(
        "[Phase3Focus] Sell price/confirmation/transaction bytecode func=%d itemCostPush=0x%X confirmPush=0x%X finalSellPush=0x%X range=0x%X-0x%X",
        sellHandler.index,
        itemCostPush,
        confirmPush,
        finalSellPush,
        requestedStart,
        requestedEnd);

    for (uint32_t position = sellHandler.start;
         position < sellHandler.end;)
    {
        uint32_t length = 0;
        unsigned char* op = ScriptCodePointer(program, position);
        if (!op || !GetVmInstructionLength(program, position, length))
        {
            Logf(
                "[Phase3Focus] parser failed pc=0x%X",
                position);
            return;
        }

        if (position >= requestedStart && position < requestedEnd)
            LogPhase3FocusedInstruction(program, position, length, functions);

        position += length;
    }

    Logf("[Phase3Focus] END");
}

static void RunPhase3NativeProbe(
    size_t slot,
    Phase3NativeCallContext* context);

static void Phase3NativeProbe0(Phase3NativeCallContext* context)
{
    RunPhase3NativeProbe(0, context);
}

static void Phase3NativeProbe1(Phase3NativeCallContext* context)
{
    RunPhase3NativeProbe(1, context);
}

static void Phase3NativeProbe2(Phase3NativeCallContext* context)
{
    RunPhase3NativeProbe(2, context);
}

static void Phase3NativeProbe3(Phase3NativeCallContext* context)
{
    RunPhase3NativeProbe(3, context);
}

static void Phase3NativeProbe4(Phase3NativeCallContext* context)
{
    RunPhase3NativeProbe(4, context);
}

static void Phase3NativeProbe5(Phase3NativeCallContext* context)
{
    RunPhase3NativeProbe(5, context);
}

static void Phase3NativeProbe6(Phase3NativeCallContext* context)
{
    RunPhase3NativeProbe(6, context);
}

static void Phase3NativeProbe7(Phase3NativeCallContext* context)
{
    RunPhase3NativeProbe(7, context);
}

static void Phase3NativeProbe8(Phase3NativeCallContext* context)
{
    RunPhase3NativeProbe(8, context);
}

static void Phase3NativeProbe9(Phase3NativeCallContext* context)
{
    RunPhase3NativeProbe(9, context);
}

static void Phase3HelperNetworkProbe(Phase3NativeCallContext* context);

static Phase3NativeProbe g_phase3NativeProbes[] =
{
    { 0xFFFF, 0, 0, 0, "pre-network vehicle gate", nullptr, &Phase3NativeProbe0, false, 0, 0, 0 },
    { 0xFFFF, 0, 0, 0, "vehicle/network fallback gate", nullptr, &Phase3NativeProbe1, false, 0, 0, 0 },
    { 0xFFFF, 0, 0, 0, "state gate A", nullptr, &Phase3NativeProbe2, false, 0, 0, 0 },
    { 0xFFFF, 0, 0, 0, "state gate B", nullptr, &Phase3NativeProbe3, false, 0, 0, 0 },
    { 0xFFFF, 0, 0, 0, "later state gate C", nullptr, &Phase3NativeProbe4, false, 0, 0, 0 },
    { 0xFFFF, 0, 0, 0, "later state gate D", nullptr, &Phase3NativeProbe5, false, 0, 0, 0 },
    { 0xFFFF, 0, 0, 0, "state/bit test path A", nullptr, &Phase3NativeProbe6, false, 0, 0, 0 },
    { 0xFFFF, 0, 0, 0, "string-state gate", nullptr, &Phase3NativeProbe7, false, 0, 0, 0 },
    { 0xFFFF, 0, 0, 0, "bit/state update", nullptr, &Phase3NativeProbe8, false, 0, 0, 0 },
    { 0xFFFF, 0, 0, 0, "delay/yield action", nullptr, &Phase3NativeProbe9, false, 0, 0, 0 },
};

static constexpr size_t kPhase3NativeProbeCount =
    sizeof(g_phase3NativeProbes) / sizeof(g_phase3NativeProbes[0]);


static bool MatchSellPriceArraySequence(
    Phase2ScrProgram* program,
    uint32_t position,
    bool requireLoad,
    uint16_t& staticBaseIndex,
    int16_t& fieldOffset,
    uint8_t& arrayStride,
    uint32_t& sequenceEnd)
{
    staticBaseIndex = 0;
    fieldOffset = 0;
    arrayStride = 0;
    sequenceEnd = position;

    unsigned char* pushIndex =
        ScriptCodePointer(program, position);
    if (!pushIndex || *pushIndex != kVmPushConst0)
        return false;

    const uint32_t staticPosition = position + 1;
    unsigned char* staticOp =
        ScriptCodePointer(program, staticPosition);
    if (!staticOp || *staticOp != kVmStaticU16)
        return false;

    uint32_t staticBaseRaw = 0;
    if (!ReadScriptUnsigned(
            program,
            staticPosition + 1,
            2,
            staticBaseRaw))
    {
        return false;
    }

    const uint32_t offsetPosition = staticPosition + 3;
    unsigned char* offsetOp =
        ScriptCodePointer(program, offsetPosition);
    if (!offsetOp || *offsetOp != kVmIoffsetS16)
        return false;

    int16_t signedOffset = 0;
    if (!ReadScriptSigned16(
            program,
            offsetPosition + 1,
            signedOffset))
    {
        return false;
    }

    const uint32_t arrayPosition = offsetPosition + 3;
    unsigned char* arrayOp =
        ScriptCodePointer(program, arrayPosition);

    if (!arrayOp
        || (requireLoad
            ? *arrayOp != kVmArrayU8Load
            : *arrayOp != kVmArrayU8))
    {
        return false;
    }

    uint32_t strideRaw = 0;
    if (!ReadScriptUnsigned(
            program,
            arrayPosition + 1,
            1,
            strideRaw)
        || strideRaw == 0
        || strideRaw > 0xFFU)
    {
        return false;
    }

    staticBaseIndex =
        static_cast<uint16_t>(staticBaseRaw);
    fieldOffset = signedOffset;
    arrayStride =
        static_cast<uint8_t>(strideRaw);
    sequenceEnd = arrayPosition + 2;
    return true;
}

static bool ResolvePhase3SellPricePath(
    Phase2ScrProgram* program,
    const VmFunctionRange& sellHandler,
    const std::vector<VmFunctionRange>& functions)
{
    g_phase3SellPricePath = Phase3SellPricePath{};

    if (!program || !sellHandler.found)
        return false;

    std::vector<uint32_t> itemCostOffsets;
    FindExactScriptStringOffsets(
        program,
        "ITEM_COST",
        itemCostOffsets);

    if (itemCostOffsets.empty())
    {
        Logf(
            "[Phase3L] SellPricePath=no reason=ITEM_COST string not found");
        return false;
    }

    uint32_t itemCostPush = 0;
    int itemCostReferences = 0;

    for (uint32_t position = sellHandler.start;
         position < sellHandler.end;)
    {
        uint32_t length = 0;
        unsigned char* op =
            ScriptCodePointer(program, position);

        if (!op
            || !GetVmInstructionLength(
                program,
                position,
                length))
        {
            Logf(
                "[Phase3L] SellPricePath=no reason=parser failure pc=0x%X",
                position);
            return false;
        }

        int pushedValue = -1;
        if (TryGetVmPushedInt(
                program,
                position,
                pushedValue)
            && pushedValue >= 0)
        {
            unsigned char* next =
                ScriptCodePointer(
                    program,
                    position + length);

            if (next && *next == kVmString)
            {
                for (size_t i = 0;
                     i < itemCostOffsets.size();
                     ++i)
                {
                    if (itemCostOffsets[i]
                        == static_cast<uint32_t>(
                            pushedValue))
                    {
                        ++itemCostReferences;
                        itemCostPush = position;
                        break;
                    }
                }
            }
        }

        position += length;
    }

    if (itemCostReferences != 1
        || itemCostPush == 0)
    {
        Logf(
            "[Phase3L] SellPricePath=no reason=ITEM_COST code reference not unique references=%d",
            itemCostReferences);
        return false;
    }

    uint32_t priceLoadPosition = 0;
    uint16_t staticBaseIndex = 0;
    int16_t fieldOffset = 0;
    uint8_t arrayStride = 0;

    const uint32_t loadSearchEnd =
        itemCostPush + 0x40U < sellHandler.end
        ? itemCostPush + 0x40U
        : sellHandler.end;

    for (uint32_t position = itemCostPush;
         position < loadSearchEnd;)
    {
        uint32_t length = 0;
        if (!GetVmInstructionLength(
                program,
                position,
                length))
        {
            return false;
        }

        uint16_t candidateBase = 0;
        int16_t candidateOffset = 0;
        uint8_t candidateStride = 0;
        uint32_t sequenceEnd = 0;

        if (MatchSellPriceArraySequence(
                program,
                position,
                true,
                candidateBase,
                candidateOffset,
                candidateStride,
                sequenceEnd))
        {
            priceLoadPosition = position;
            staticBaseIndex = candidateBase;
            fieldOffset = candidateOffset;
            arrayStride = candidateStride;
            break;
        }

        position += length;
    }

    if (priceLoadPosition == 0)
    {
        Logf(
            "[Phase3L] SellPricePath=no reason=price array load not found after ITEM_COST");
        return false;
    }

    uint32_t registerNativeSite = 0;
    uint16_t registerNativeIndex = 0xFFFF;
    int registerNativeMatches = 0;

    // Rockstar registers sData.iOptionCost[0] immediately after assigning the
    // Sell value and before composing the visible ITEM_COST text. Resolve that
    // exact SECURITY::REGISTER_SCRIPT_VARIABLE use by matching the same
    // iOptionCost[0] address expression followed by a one-argument/zero-return
    // NATIVE. No native hash or build-specific native index is hardcoded.
    for (uint32_t position = sellHandler.start;
         position < itemCostPush;)
    {
        uint32_t length = 0;
        if (!GetVmInstructionLength(
                program,
                position,
                length))
        {
            return false;
        }

        uint16_t candidateBase = 0;
        int16_t candidateOffset = 0;
        uint8_t candidateStride = 0;
        uint32_t sequenceEnd = 0;

        if (MatchSellPriceArraySequence(
                program,
                position,
                false,
                candidateBase,
                candidateOffset,
                candidateStride,
                sequenceEnd)
            && candidateBase == staticBaseIndex
            && candidateOffset == fieldOffset
            && candidateStride == arrayStride)
        {
            unsigned char* nativeOp =
                ScriptCodePointer(
                    program,
                    sequenceEnd);

            if (nativeOp
                && *nativeOp == kVmNative)
            {
                uint8_t packed = 0;
                uint16_t nativeIndex = 0;

                if (ReadVmNativeSignature(
                        program,
                        sequenceEnd,
                        packed,
                        nativeIndex)
                    && (packed >> 2) == 1
                    && (packed & 0x03) == 0)
                {
                    ++registerNativeMatches;
                    registerNativeSite =
                        sequenceEnd;
                    registerNativeIndex =
                        nativeIndex;
                }
            }
        }

        position += length;
    }

    if (registerNativeMatches != 1
        || registerNativeSite == 0
        || registerNativeIndex == 0xFFFF)
    {
        Logf(
            "[Phase3L] SellPricePath=no reason=price REGISTER_SCRIPT_VARIABLE site not unique matches=%d",
            registerNativeMatches);
        return false;
    }

    uint32_t priceAddressPosition = 0;
    uint32_t initializerCall = 0;
    VmFunctionRange initializerFunction{};

    const uint32_t addressSearchStart =
        itemCostPush > sellHandler.start + 0x80U
        ? itemCostPush - 0x80U
        : sellHandler.start;

    for (uint32_t position = addressSearchStart;
         position < itemCostPush;)
    {
        uint32_t length = 0;
        if (!GetVmInstructionLength(
                program,
                position,
                length))
        {
            return false;
        }

        uint16_t candidateBase = 0;
        int16_t candidateOffset = 0;
        uint8_t candidateStride = 0;
        uint32_t sequenceEnd = 0;

        if (MatchSellPriceArraySequence(
                program,
                position,
                false,
                candidateBase,
                candidateOffset,
                candidateStride,
                sequenceEnd)
            && candidateBase == staticBaseIndex
            && candidateOffset == fieldOffset
            && candidateStride == arrayStride)
        {
            unsigned char* push0A =
                ScriptCodePointer(
                    program,
                    sequenceEnd);
            unsigned char* push0B =
                ScriptCodePointer(
                    program,
                    sequenceEnd + 1);
            unsigned char* callOp =
                ScriptCodePointer(
                    program,
                    sequenceEnd + 2);

            if (push0A
                && push0B
                && callOp
                && *push0A == kVmPushConst0
                && *push0B == kVmPushConst0
                && *callOp == kVmCall)
            {
                uint32_t target = 0;
                if (ReadScriptUnsigned(
                        program,
                        sequenceEnd + 3,
                        3,
                        target))
                {
                    const VmFunctionRange* targetFunction =
                        FindVmFunctionByStart(
                            functions,
                            target);

                    if (targetFunction)
                    {
                        priceAddressPosition = position;
                        initializerCall = sequenceEnd + 2;
                        initializerFunction =
                            *targetFunction;
                    }
                }
            }
        }

        position += length;
    }

    if (priceAddressPosition == 0
        || initializerCall == 0
        || !initializerFunction.found)
    {
        Logf(
            "[Phase3L] SellPricePath=no reason=price initializer call not found");
        return false;
    }

    int matchingPriceFieldUses = 0;

    for (uint32_t position = sellHandler.start;
         position < sellHandler.end;)
    {
        uint32_t length = 0;
        if (!GetVmInstructionLength(
                program,
                position,
                length))
        {
            return false;
        }

        uint16_t candidateBase = 0;
        int16_t candidateOffset = 0;
        uint8_t candidateStride = 0;
        uint32_t sequenceEnd = 0;

        if ((MatchSellPriceArraySequence(
                program,
                position,
                true,
                candidateBase,
                candidateOffset,
                candidateStride,
                sequenceEnd)
             || MatchSellPriceArraySequence(
                program,
                position,
                false,
                candidateBase,
                candidateOffset,
                candidateStride,
                sequenceEnd))
            && candidateBase == staticBaseIndex
            && candidateOffset == fieldOffset
            && candidateStride == arrayStride)
        {
            ++matchingPriceFieldUses;
        }

        position += length;
    }

    if (matchingPriceFieldUses < 3)
    {
        Logf(
            "[Phase3L] SellPricePath=no reason=price field validation weak uses=%d",
            matchingPriceFieldUses);
        return false;
    }

    const int64_t element0Index64 =
        static_cast<int64_t>(staticBaseIndex)
        + static_cast<int64_t>(fieldOffset)
        + static_cast<int64_t>(arrayStride);

    if (element0Index64 < 0
        || element0Index64 > 0xFFFFFFFFLL)
    {
        Logf(
            "[Phase3L] SellPricePath=no reason=derived static index invalid index=%lld",
            static_cast<long long>(
                element0Index64));
        return false;
    }

    g_phase3SellPricePath.resolved = true;
    g_phase3SellPricePath.staticBaseIndex =
        staticBaseIndex;
    g_phase3SellPricePath.fieldOffset =
        fieldOffset;
    g_phase3SellPricePath.arrayStride =
        arrayStride;
    g_phase3SellPricePath.element0StaticIndex =
        static_cast<uint32_t>(
            element0Index64);
    g_phase3SellPricePath.itemCostPush =
        itemCostPush;
    g_phase3SellPricePath.priceLoadPosition =
        priceLoadPosition;
    g_phase3SellPricePath.priceAddressPosition =
        priceAddressPosition;
    g_phase3SellPricePath.initializerCall =
        initializerCall;
    g_phase3SellPricePath.initializerFunction =
        initializerFunction;
    g_phase3SellPricePath.registerNativeSite =
        registerNativeSite;
    g_phase3SellPricePath.registerNativeIndex =
        registerNativeIndex;

    Logf(
        "[Phase3L] SellPricePath=yes itemCostPush=0x%X priceLoad=0x%X priceAddress=0x%X initializerCall=0x%X initializerFunc=%d@0x%X registerSite=0x%X registerNativeIndex=%u staticBase=%u fieldOffset=%d stride=%u element0StaticIndex=%u validatedUses=%d",
        itemCostPush,
        priceLoadPosition,
        priceAddressPosition,
        initializerCall,
        initializerFunction.index,
        initializerFunction.start,
        registerNativeSite,
        static_cast<unsigned int>(
            registerNativeIndex),
        static_cast<unsigned int>(
            staticBaseIndex),
        static_cast<int>(fieldOffset),
        static_cast<unsigned int>(
            arrayStride),
        static_cast<unsigned int>(
            g_phase3SellPricePath
                .element0StaticIndex),
        matchingPriceFieldUses);

    std::vector<Phase3DirectCall> calls;
    if (!CollectDirectCalls(
            program,
            initializerFunction,
            functions,
            calls))
    {
        Logf(
            "[Phase3L] Price initializer CALL analysis failed safely");
        return true;
    }

    Logf(
        "[Phase3L] PriceInitializer func=%d start=0x%X end=0x%X size=%u args=%u calls=%u",
        initializerFunction.index,
        initializerFunction.start,
        initializerFunction.end,
        static_cast<unsigned int>(
            initializerFunction.end
            - initializerFunction.start),
        static_cast<unsigned int>(
            initializerFunction.argCount),
        static_cast<unsigned int>(
            calls.size()));

    for (uint32_t position =
             initializerFunction.start;
         position < initializerFunction.end;)
    {
        uint32_t length = 0;
        if (!GetVmInstructionLength(
                program,
                position,
                length))
        {
            Logf(
                "[Phase3L] PriceInitializer parser failed pc=0x%X",
                position);
            break;
        }

        LogPhase3FocusedInstruction(
            program,
            position,
            length,
            functions);

        position += length;
    }

    for (size_t i = 0; i < calls.size(); ++i)
    {
        if (!calls[i].targetFunction)
            continue;

        const int networkRefs =
            g_phase2NetworkGameNativeIndex
                != 0xFFFF
            ? CountNativeIndexReferences(
                program,
                *calls[i].targetFunction,
                g_phase2NetworkGameNativeIndex,
                nullptr)
            : -1;

        Logf(
            "[Phase3L] PriceInitializerCall ordinal=%u call=0x%X targetFunc=%d targetStart=0x%X size=%u networkGameRefs=%d",
            static_cast<unsigned int>(i),
            calls[i].callPosition,
            calls[i].targetFunction->index,
            calls[i].targetFunction->start,
            static_cast<unsigned int>(
                calls[i].targetFunction->end
                - calls[i].targetFunction->start),
            networkRefs);

        if (networkRefs <= 0)
            continue;

        const uint32_t helperSize =
            calls[i].targetFunction->end
            - calls[i].targetFunction->start;

        if (helperSize > 512U)
        {
            Logf(
                "[Phase3L] PriceGateHelper dump skipped func=%d reason=size=%u",
                calls[i].targetFunction->index,
                helperSize);
            continue;
        }

        Logf(
            "[Phase3L] PriceGateHelper begin func=%d start=0x%X end=0x%X networkGameRefs=%d",
            calls[i].targetFunction->index,
            calls[i].targetFunction->start,
            calls[i].targetFunction->end,
            networkRefs);

        for (uint32_t helperPosition =
                 calls[i].targetFunction->start;
             helperPosition
                 < calls[i].targetFunction->end;)
        {
            uint32_t helperLength = 0;
            if (!GetVmInstructionLength(
                    program,
                    helperPosition,
                    helperLength))
            {
                break;
            }

            LogPhase3FocusedInstruction(
                program,
                helperPosition,
                helperLength,
                functions);

            helperPosition += helperLength;
        }
    }

    return true;
}

static void LogPhase3SellPriceState(
    const char* reason)
{
    if (!g_phase3SellPricePath.resolved
        || !g_carmodShopActive
        || NETWORK::NETWORK_IS_GAME_IN_PROGRESS())
    {
        return;
    }

    Phase2ThreadInfo threadInfo{};
    if (!GetPhase2ThreadInfo(
            kCarmodShopHash,
            threadInfo)
        || !threadInfo.stack)
    {
        Logf(
            "[Phase3L] PriceState reason=%s readable=no",
            reason ? reason : "unspecified");
        return;
    }

    const uint32_t index =
        g_phase3SellPricePath
            .element0StaticIndex;

    if (index >= threadInfo.stackSize)
    {
        Logf(
            "[Phase3L] PriceState reason=%s readable=no index=%u stackSize=%u",
            reason ? reason : "unspecified",
            static_cast<unsigned int>(
                index),
            static_cast<unsigned int>(
                threadInfo.stackSize));
        return;
    }

    const unsigned char* slot =
        reinterpret_cast<const unsigned char*>(
            threadInfo.stack)
        + static_cast<size_t>(index)
            * sizeof(uintptr_t);

    uint64_t raw = 0;
    if (!IsReadableMemory(
            slot,
            sizeof(raw)))
    {
        Logf(
            "[Phase3L] PriceState reason=%s readable=no index=%u",
            reason ? reason : "unspecified",
            static_cast<unsigned int>(
                index));
        return;
    }

    std::memcpy(
        &raw,
        slot,
        sizeof(raw));

    Logf(
        "[Phase3L] PriceState reason=%s index=%u raw=0x%llX low32=%d",
        reason ? reason : "unspecified",
        static_cast<unsigned int>(index),
        static_cast<unsigned long long>(
            raw),
        static_cast<int32_t>(
            raw & 0xFFFFFFFFULL));
}

static int GetPhase3VehicleModelValue(Hash model)
{
    nativeInit(kGetVehicleModelValueNative);
    nativePush64(
        static_cast<uint64_t>(
            static_cast<uint32_t>(model)));

    uint64_t* result = nativeCall();
    if (!result)
        return 0;

    return static_cast<int32_t>(
        *result & 0xFFFFFFFFULL);
}

static bool TryGetPhase3SwitchStaticIndex(
    Phase2ScrProgram* program,
    const std::vector<uint32_t>& instructionPositions,
    size_t switchInstructionIndex,
    uint32_t& staticIndex)
{
    staticIndex = 0;

    if (!program
        || switchInstructionIndex == 0
        || switchInstructionIndex
            > instructionPositions.size())
    {
        return false;
    }

    // The value feeding SWITCH must be immediately before it. Decode that
    // expression backwards instead of grabbing only the final IOFFSET load.
    //
    // Example from Legacy build 93 DO_STAGE_SELL:
    //   STATIC_U16       0x02F1  (753)
    //   IOFFSET_U8       0x77     (+119)
    //   IOFFSET_U8_LOAD  0x05     (+5)
    //   SWITCH
    // The correct iControl slot is therefore 877, not 758.
    int64_t accumulatedOffset = 0;
    bool sawOffsetExpression = false;

    const size_t firstIndex =
        switchInstructionIndex > 12
            ? switchInstructionIndex - 12
            : 0;

    for (size_t i = switchInstructionIndex;
         i-- > firstIndex;)
    {
        const uint32_t position =
            instructionPositions[i];

        unsigned char* op =
            ScriptCodePointer(
                program,
                position);

        if (!op)
            return false;

        // Direct static load feeding SWITCH.
        if (!sawOffsetExpression
            && (*op == 0x50
                || *op == 0x5F))
        {
            uint32_t directIndex = 0;
            if (!ReadScriptUnsigned(
                    program,
                    position + 1,
                    *op == 0x50 ? 2U : 3U,
                    directIndex))
            {
                return false;
            }

            staticIndex = directIndex;
            return true;
        }

        // Address-offset and address-offset-load instructions all contribute
        // to the final static address. Keep walking until the STATIC base.
        if (*op == 0x40
            || *op == 0x41)
        {
            uint32_t rawOffset = 0;
            if (!ReadScriptUnsigned(
                    program,
                    position + 1,
                    1,
                    rawOffset))
            {
                return false;
            }

            accumulatedOffset +=
                static_cast<int64_t>(
                    rawOffset);
            sawOffsetExpression = true;
            continue;
        }

        if (*op == 0x46
            || *op == 0x47)
        {
            int16_t rawOffset = 0;
            if (!ReadScriptSigned16(
                    program,
                    position + 1,
                    rawOffset))
            {
                return false;
            }

            accumulatedOffset +=
                static_cast<int64_t>(
                    rawOffset);
            sawOffsetExpression = true;
            continue;
        }

        if (sawOffsetExpression
            && (*op == 0x4F
                || *op == 0x5E))
        {
            uint32_t baseStatic = 0;
            if (!ReadScriptUnsigned(
                    program,
                    position + 1,
                    *op == 0x4F ? 2U : 3U,
                    baseStatic))
            {
                return false;
            }

            const int64_t resolved =
                static_cast<int64_t>(
                    baseStatic)
                + accumulatedOffset;

            if (resolved < 0
                || resolved > 0xFFFFFF)
            {
                return false;
            }

            staticIndex =
                static_cast<uint32_t>(
                    resolved);
            return true;
        }

        // Once an offset expression has started, any unrelated opcode means
        // this is not the simple static-address chain we require.
        if (sawOffsetExpression)
            return false;

        // Before the expression starts, do not wander arbitrarily far through
        // unrelated stack operations.
        return false;
    }

    return false;
}

static bool Phase3RangeReferencesAnyString(
    Phase2ScrProgram* program,
    uint32_t start,
    uint32_t end,
    const std::vector<uint32_t>& stringOffsets)
{
    if (!program
        || stringOffsets.empty()
        || end <= start)
    {
        return false;
    }

    for (uint32_t position = start;
         position < end;)
    {
        uint32_t length = 0;
        if (!GetVmInstructionLength(
                program,
                position,
                length))
        {
            return false;
        }

        int pushedValue = -1;
        if (TryGetVmPushedInt(
                program,
                position,
                pushedValue)
            && pushedValue >= 0)
        {
            unsigned char* next =
                ScriptCodePointer(
                    program,
                    position + length);

            if (next && *next == kVmString)
            {
                const uint32_t value =
                    static_cast<uint32_t>(
                        pushedValue);

                for (size_t i = 0;
                     i < stringOffsets.size();
                     ++i)
                {
                    if (stringOffsets[i] == value)
                        return true;
                }
            }
        }

        position += length;
    }

    return false;
}

static bool ResolvePhase3SellStagePath(
    Phase2ScrProgram* program,
    const std::vector<VmFunctionRange>& functions,
    const VmFunctionRange& sellHandler)
{
    g_phase3SellStagePath =
        Phase3SellStagePath{};

    if (!program
        || !sellHandler.found)
    {
        return false;
    }

    uint32_t resolvedStaticIndex = 0;
    int resolvedSellMenuValue = -1;
    uint32_t resolvedSwitch = 0;
    uint32_t resolvedCall = 0;
    int resolvedFunctionIndex = -1;
    int qualifiedCount = 0;

    for (size_t functionIndex = 0;
         functionIndex < functions.size();
         ++functionIndex)
    {
        const VmFunctionRange& function =
            functions[functionIndex];

        std::vector<Phase3DirectCall> calls;
        if (!CollectDirectCalls(
                program,
                function,
                functions,
                calls))
        {
            return false;
        }

        for (size_t callIndex = 0;
             callIndex < calls.size();
             ++callIndex)
        {
            const Phase3DirectCall& call =
                calls[callIndex];

            if (call.target != sellHandler.start)
                continue;

            std::vector<uint32_t> instructionPositions;
            for (uint32_t position = function.start;
                 position < function.end;)
            {
                uint32_t length = 0;
                if (!GetVmInstructionLength(
                        program,
                        position,
                        length))
                {
                    return false;
                }

                instructionPositions.push_back(
                    position);
                position += length;
            }

            for (size_t instructionIndex = 0;
                 instructionIndex
                    < instructionPositions.size();
                 ++instructionIndex)
            {
                const uint32_t switchPosition =
                    instructionPositions[
                        instructionIndex];

                unsigned char* op =
                    ScriptCodePointer(
                        program,
                        switchPosition);

                if (!op || *op != kVmSwitch)
                    continue;

                unsigned char* countPtr =
                    ScriptCodePointer(
                        program,
                        switchPosition + 1);

                if (!countPtr)
                    continue;

                const uint32_t caseCount =
                    static_cast<uint32_t>(
                        *countPtr);

                // The main carmod menu dispatch has many cases. Requiring a
                // reasonably large switch prevents a nested state machine from
                // being mistaken for the eMenu dispatcher.
                if (caseCount < 16)
                    continue;

                struct MenuCase
                {
                    int value;
                    uint32_t target;
                };

                std::vector<MenuCase> cases;
                cases.reserve(caseCount);

                bool validSwitch = true;

                for (uint32_t entry = 0;
                     entry < caseCount;
                     ++entry)
                {
                    const uint32_t entryPosition =
                        switchPosition + 2
                        + entry * 6;

                    uint32_t caseValueRaw = 0;
                    uint32_t relativeRaw = 0;

                    if (!ReadScriptUnsigned(
                            program,
                            entryPosition,
                            4,
                            caseValueRaw)
                        || !ReadScriptUnsigned(
                            program,
                            entryPosition + 4,
                            2,
                            relativeRaw))
                    {
                        validSwitch = false;
                        break;
                    }

                    const int16_t relative =
                        static_cast<int16_t>(
                            relativeRaw & 0xFFFFU);

                    const int64_t target64 =
                        static_cast<int64_t>(
                            entryPosition)
                        + 6
                        + static_cast<int64_t>(
                            relative);

                    if (target64
                            < static_cast<int64_t>(
                                function.start)
                        || target64
                            >= static_cast<int64_t>(
                                function.end))
                    {
                        validSwitch = false;
                        break;
                    }

                    MenuCase menuCase{};
                    menuCase.value =
                        static_cast<int32_t>(
                            caseValueRaw);
                    menuCase.target =
                        static_cast<uint32_t>(
                            target64);
                    cases.push_back(menuCase);
                }

                if (!validSwitch
                    || cases.empty())
                {
                    continue;
                }

                for (size_t i = 0;
                     i < cases.size();
                     ++i)
                {
                    uint32_t blockEnd =
                        function.end;

                    for (size_t j = 0;
                         j < cases.size();
                         ++j)
                    {
                        if (cases[j].target
                                > cases[i].target
                            && cases[j].target
                                < blockEnd)
                        {
                            blockEnd =
                                cases[j].target;
                        }
                    }

                    if (call.callPosition
                            < cases[i].target
                        || call.callPosition
                            >= blockEnd)
                    {
                        continue;
                    }

                    // DO_STAGE_SELL is dispatched directly from CMM_SELL.
                    // The call should be very near the case entry; this also
                    // prevents an outer switch from accidentally qualifying.
                    if (call.callPosition
                            - cases[i].target
                        > 32U)
                    {
                        continue;
                    }

                    uint32_t staticIndex = 0;
                    if (!TryGetPhase3SwitchStaticIndex(
                            program,
                            instructionPositions,
                            instructionIndex,
                            staticIndex))
                    {
                        continue;
                    }

                    ++qualifiedCount;
                    resolvedStaticIndex =
                        staticIndex;
                    resolvedSellMenuValue =
                        cases[i].value;
                    resolvedSwitch =
                        switchPosition;
                    resolvedCall =
                        call.callPosition;
                    resolvedFunctionIndex =
                        function.index;

                    Logf(
                        "[Phase3P] SellStage qualified func=%d switch=0x%X staticIndex=%u sellMenuValue=%d sellCall=0x%X cases=%u",
                        function.index,
                        switchPosition,
                        static_cast<unsigned int>(
                            staticIndex),
                        cases[i].value,
                        call.callPosition,
                        static_cast<unsigned int>(
                            caseCount));
                }
            }
        }
    }

    if (qualifiedCount != 1)
    {
        Logf(
            "[Phase3P] SellStage unresolved qualified=%d",
            qualifiedCount);
        return false;
    }

    g_phase3SellStagePath.resolved = true;
    g_phase3SellStagePath.staticIndex =
        resolvedStaticIndex;
    g_phase3SellStagePath.sellMenuValue =
        resolvedSellMenuValue;
    g_phase3SellStagePath.switchPosition =
        resolvedSwitch;
    g_phase3SellStagePath.sellCallPosition =
        resolvedCall;
    g_phase3SellStagePath.functionIndex =
        resolvedFunctionIndex;

    Logf(
        "[Phase3P] SellStage resolved staticIndex=%u sellMenuValue=%d switch=0x%X sellCall=0x%X func=%d",
        static_cast<unsigned int>(
            resolvedStaticIndex),
        resolvedSellMenuValue,
        resolvedSwitch,
        resolvedCall,
        resolvedFunctionIndex);

    return true;
}

static bool ResolvePhase3SellControlPath(
    Phase2ScrProgram* program,
    const VmFunctionRange& sellHandler)
{
    g_phase3SellControlPath =
        Phase3SellControlPath{};

    if (!program
        || !sellHandler.found
        || sellHandler.end <= sellHandler.start)
    {
        return false;
    }

    std::vector<uint32_t> confirmOffsets;
    std::vector<uint32_t> soldOffsets;

    FindExactScriptStringOffsets(
        program,
        "CMOD_SEL_CONF",
        confirmOffsets);
    FindExactScriptStringOffsets(
        program,
        "CMOD_SEL",
        soldOffsets);

    if (confirmOffsets.empty()
        || soldOffsets.empty())
    {
        Logf(
            "[Phase3P] SellControl unresolved reason=missing Sell strings confirm=%u sold=%u",
            static_cast<unsigned int>(
                confirmOffsets.size()),
            static_cast<unsigned int>(
                soldOffsets.size()));
        return false;
    }

    std::vector<uint32_t> instructionPositions;
    for (uint32_t position = sellHandler.start;
         position < sellHandler.end;)
    {
        uint32_t length = 0;
        if (!GetVmInstructionLength(
                program,
                position,
                length))
        {
            Logf(
                "[Phase3P] SellControl unresolved reason=parser-failed pc=0x%X",
                position);
            return false;
        }

        instructionPositions.push_back(
            position);
        position += length;
    }

    uint32_t resolvedStaticIndex = 0;
    uint32_t resolvedSwitch = 0;
    int qualifiedCount = 0;

    for (size_t instructionIndex = 0;
         instructionIndex
            < instructionPositions.size();
         ++instructionIndex)
    {
        const uint32_t switchPosition =
            instructionPositions[
                instructionIndex];

        unsigned char* op =
            ScriptCodePointer(
                program,
                switchPosition);

        if (!op || *op != kVmSwitch)
            continue;

        unsigned char* countPtr =
            ScriptCodePointer(
                program,
                switchPosition + 1);

        if (!countPtr
            || static_cast<uint32_t>(
                *countPtr) != 4U)
        {
            continue;
        }

        uint32_t caseTargets[4]{};
        bool foundCases[4]{};

        for (uint32_t entry = 0;
             entry < 4;
             ++entry)
        {
            const uint32_t entryPosition =
                switchPosition + 2
                + entry * 6;

            uint32_t caseValue = 0;
            uint32_t relativeRaw = 0;

            if (!ReadScriptUnsigned(
                    program,
                    entryPosition,
                    4,
                    caseValue)
                || !ReadScriptUnsigned(
                    program,
                    entryPosition + 4,
                    2,
                    relativeRaw))
            {
                continue;
            }

            if (caseValue > 3)
                continue;

            const int16_t relative =
                static_cast<int16_t>(
                    relativeRaw & 0xFFFFU);

            // GTA V SWITCH cases are six-byte entries:
            //   u32 caseValue + s16 relativeOffset
            // The jump base is the byte immediately after that entire entry.
            // Previous builds incorrectly used entryPosition + 5, which put
            // every resolved case target one byte before the real opcode.
            const int64_t target64 =
                static_cast<int64_t>(
                    entryPosition)
                + 6
                + static_cast<int64_t>(
                    relative);

            if (target64
                    >= static_cast<int64_t>(
                        sellHandler.end)
                || target64
                    < static_cast<int64_t>(
                        sellHandler.start))
            {
                continue;
            }

            caseTargets[caseValue] =
                static_cast<uint32_t>(
                    target64);
            foundCases[caseValue] = true;
        }

        if (!foundCases[0]
            || !foundCases[1]
            || !foundCases[2]
            || !foundCases[3])
        {
            continue;
        }

        uint32_t orderedTargets[4] =
        {
            caseTargets[0],
            caseTargets[1],
            caseTargets[2],
            caseTargets[3]
        };

        for (size_t i = 0; i < 4; ++i)
        {
            for (size_t j = i + 1;
                 j < 4;
                 ++j)
            {
                if (orderedTargets[j]
                    < orderedTargets[i])
                {
                    const uint32_t temp =
                        orderedTargets[i];
                    orderedTargets[i] =
                        orderedTargets[j];
                    orderedTargets[j] =
                        temp;
                }
            }
        }

        auto blockEndForTarget =
            [&](uint32_t target)
            {
                uint32_t end =
                    sellHandler.end;

                for (size_t i = 0;
                     i < 4;
                     ++i)
                {
                    if (orderedTargets[i] > target)
                    {
                        end =
                            orderedTargets[i];
                        break;
                    }
                }

                return end;
            };

        const bool case0HasConfirm =
            Phase3RangeReferencesAnyString(
                program,
                caseTargets[0],
                blockEndForTarget(
                    caseTargets[0]),
                confirmOffsets);

        const bool case2HasSold =
            Phase3RangeReferencesAnyString(
                program,
                caseTargets[2],
                blockEndForTarget(
                    caseTargets[2]),
                soldOffsets);

        if (!case0HasConfirm
            || !case2HasSold)
        {
            continue;
        }

        uint32_t staticIndex = 0;
        if (!TryGetPhase3SwitchStaticIndex(
                program,
                instructionPositions,
                instructionIndex,
                staticIndex))
        {
            Logf(
                "[Phase3P] SellControl candidate rejected switch=0x%X reason=input-static-unresolved",
                switchPosition);
            continue;
        }

        ++qualifiedCount;
        resolvedStaticIndex =
            staticIndex;
        resolvedSwitch =
            switchPosition;

        Logf(
            "[Phase3P] SellControl qualified switch=0x%X staticIndex=%u case0=0x%X case1=0x%X case2=0x%X case3=0x%X",
            switchPosition,
            static_cast<unsigned int>(
                staticIndex),
            caseTargets[0],
            caseTargets[1],
            caseTargets[2],
            caseTargets[3]);
    }

    if (qualifiedCount != 1)
    {
        Logf(
            "[Phase3P] SellControl unresolved qualified=%d",
            qualifiedCount);
        return false;
    }

    g_phase3SellControlPath.resolved = true;
    g_phase3SellControlPath.staticIndex =
        resolvedStaticIndex;
    g_phase3SellControlPath.switchPosition =
        resolvedSwitch;

    Logf(
        "[Phase3P] SellControl resolved staticIndex=%u switch=0x%X states=0:select,1:confirm,2:accepted,3:complete",
        static_cast<unsigned int>(
            resolvedStaticIndex),
        resolvedSwitch);

    return true;
}

static void UpdatePhase3SellControlStateFast();

struct Phase3GtacarsPriceEntry
{
    uint32_t modelHash;
    int purchasePrice;
};

// GTACars-derived native GTA 5/Online acquisition-price reference.
// The table is keyed by the actual GTA model hash for O(log n) lookup.
// Missing/newer models safely fall back to the dynamic class/model estimator.
static const Phase3GtacarsPriceEntry kPhase3GtacarsPrices[] =
{
    { 0x00675ED7U, 210000 }, // chimera
    { 0x00ABB0C0U, 40000 }, // carbonrs
    { 0x00E83C17U, 535000 }, // hermes
    { 0x00FDFFB0U, 165000 }, // virgo3
    { 0x0239E390U, 90000 }, // hotknife
    { 0x0350D1ABU, 5000 }, // faggio2
    { 0x03E5F6B8U, 16000 }, // youga
    { 0x0409D787U, 700000 }, // yosemite3
    { 0x047A6BC1U, 200000 }, // glendale
    { 0x04CE68ACU, 35000 }, // dominator
    { 0x04F48FC4U, 1175000 }, // rebla
    { 0x05283265U, 95000 }, // bf400
    { 0x05852838U, 51000 }, // kalahari
    { 0x0612F4B6U, 550000 }, // trophytruck
    { 0x067BC037U, 138000 }, // coquette
    { 0x06FF6914U, 750000 }, // btype
    { 0x097E5533U, 1150000 }, // ardent
    { 0x09D80F93U, 1700000 }, // miljet
    { 0x0A90ED5CU, 1225000 }, // phantom3
    { 0x0BBA2261U, 904000 }, // elegy
    { 0x0D4E5F4DU, 865000 }, // cheetah2
    { 0x0D4EA603U, 490000 }, // sabregt2
    { 0x0DC60D2BU, 325000 }, // speeder
    { 0x0DF381E5U, 1595000 }, // reaper
    { 0x0E2C013EU, 535000 }, // buffalo3
    { 0x1044926FU, 1329000 }, // tempesta
    { 0x1149422FU, 22000 }, // tropic
    { 0x11962E49U, 3870000 }, // annihilator2
    { 0x11AA0E14U, 864500 }, // gburrito2
    { 0x11CBC051U, 192000 }, // verus
    { 0x11F58A5AU, 670000 }, // stryder
    { 0x11F76C14U, 15000 }, // hexer
    { 0x127E90D5U, 450000 }, // dynasty
    { 0x1324E960U, 1272000 }, // stafford
    { 0x132D5A1AU, 225000 }, // crusader
    { 0x13B57D8AU, 185000 }, // cogcabrio
    { 0x142E0DC3U, 240000 }, // vacca
    { 0x1446590AU, 3515000 }, // formula
    { 0x149BD32AU, 18420500 }, // pbus2
    { 0x14D22159U, 230000 }, // gauntlet2
    { 0x14D69010U, 225000 }, // chino
    { 0x1573422DU, 890000 }, // baller7
    { 0x163F8520U, 13218750 }, // slamvan5
    { 0x16E478C1U, 110000 }, // surano
    { 0x171C92C4U, 1400000 }, // hauler2
    { 0x17420102U, 225000 }, // cliffhanger
    { 0x177DA45CU, 1470000 }, // jb7002
    { 0x17DF5EC2U, 1966210 }, // squalo
    { 0x185484E1U, 500000 }, // turismor
    { 0x185E2FF3U, 1268000 }, // outlaw
    { 0x18619B7EU, 580000 }, // kanjo
    { 0x187D938DU, 6982500 }, // kuruma2
    { 0x18F25AC7U, 440000 }, // infernus
    { 0x196F9418U, 1775000 }, // dominator7
    { 0x19DD9ED1U, 1245000 }, // nightshark
    { 0x1A79847AU, 598500 }, // boxville4
    { 0x1A861243U, 22849400 }, // imperator
    { 0x1AAD0DEDU, 3724000 }, // volatol
    { 0x1ABA13B5U, 8000 }, // cruiser
    { 0x1B8165D3U, 1650000 }, // jubilee
    { 0x1BB290BCU, 30000 }, // tornado
    { 0x1BF8D381U, 865000 }, // lguard
    { 0x1C09CF5EU, 374000 }, // baller5
    { 0x1CBDC10BU, 1735000 }, // lynx
    { 0x1D06D681U, 195000 }, // huntley
    { 0x1DC0BA53U, 36000 }, // fusilade
    { 0x1DD4C0FFU, 909000 }, // swinger
    { 0x1F3766E3U, 55000 }, // voodoo2
    { 0x1F52A43FU, 325000 }, // moonbeam
    { 0x20314B42U, 21386400 }, // zr380
    { 0x206D1B68U, 100000 }, // khamelion
    { 0x2189D250U, 30922500 }, // apc
    { 0x2290C50AU, 1260000 }, // warrener2
    { 0x23CA25F2U, 625000 }, // hustler
    { 0x250B0C5EU, 1625000 }, // luxor
    { 0x2560B2FCU, 45000 }, // romero
    { 0x25676EAFU, 135000 }, // fcr
    { 0x256E92BAU, 1089000 }, // issi4
    { 0x258C9364U, 1580000 }, // astron
    { 0x25C5AF13U, 565000 }, // banshee2
    { 0x25CBE2E2U, 247000 }, // baller4
    { 0x26321E67U, 9975000 }, // lectro
    { 0x2714AA93U, 2820000 }, // zeno
    { 0x276D98A3U, 1145000 }, // comet5
    { 0x27816B7EU, 1720000 }, // iwagen
    { 0x27B4E6B0U, 513000 }, // baller6
    { 0x27D79225U, 1609000 }, // bruiser
    { 0x287FA449U, 38703000 }, // cerberus2
    { 0x28AD20E1U, 2926000 }, // boxville5
    { 0x28B67ACAU, 250000 }, // contender
    { 0x28EAB80FU, 718000 }, // drafter
    { 0x29B0DA97U, 11000 }, // surfer
    { 0x29FCD3E4U, 396000 }, // cog552
    { 0x2A54C47DU, 2113000 }, // supervolito
    { 0x2AE524A8U, 430000 }, // ruston
    { 0x2B0C4DCDU, 615000 }, // gauntlet3
    { 0x2B26F456U, 62000 }, // dukes
    { 0x2B7F9DE3U, 495000 }, // slamvan
    { 0x2BE8B90AU, 1220000 }, // dominator8
    { 0x2BEC3CBEU, 96000 }, // buffalo2
    { 0x2C1FEA99U, 2214000 }, // vagrant
    { 0x2C2C2324U, 120000 }, // gargoyle
    { 0x2C509634U, 90000 }, // sovereign
    { 0x2C634FBDU, 1300000 }, // frogger
    { 0x2D3BD401U, 950000 }, // ztype
    { 0x2DB8D1AAU, 150000 }, // alpha
    { 0x2EA68690U, 1500000 }, // rhino
    { 0x2EC385FEU, 695000 }, // coquette3
    { 0x2EF89E46U, 7000 }, // sanchez
    { 0x2F03547BU, 1750000 }, // buzzard
    { 0x30D3F6D8U, 1995000 }, // sheava
    { 0x30FF0190U, 412000 }, // defiler
    { 0x31F0B376U, 1825000 }, // annihilator
    { 0x3201DD49U, 900000 }, // z190
    { 0x32174AFCU, 15308750 }, // monster4
    { 0x322CF98FU, 140000 }, // rhapsody
    { 0x32B29A4BU, 27000 }, // bjxl
    { 0x33581161U, 299000 }, // jetmax
    { 0x33B98FE2U, 1420000 }, // pariah
    { 0x3404691CU, 1718000 }, // sultan2
    { 0x3412AE2DU, 95000 }, // sentinel2
    { 0x34B7390FU, 42000 }, // habanero
    { 0x34B82784U, 35245000 }, // oppressor
    { 0x34DBA661U, 31853500 }, // stromberg
    { 0x34DD8AA1U, 16000 }, // intruder
    { 0x35DED0DDU, 990000 }, // savestra
    { 0x360A438EU, 154000 }, // cog55
    { 0x36A167E0U, 925000 }, // rrocket
    { 0x36B4A8A9U, 2375000 }, // xa21
    { 0x378236E1U, 360000 }, // issi3
    { 0x381E10BDU, 57456000 }, // ruiner2
    { 0x3822BDFEU, 9044000 }, // casco
    { 0x3944D5A0U, 2740000 }, // furia
    { 0x39D6779EU, 275000 }, // duster
    { 0x39D6E83FU, 3990000 }, // hydra
    { 0x39DA2754U, 12000 }, // sultan
    { 0x39F9C898U, 375000 }, // tampa
    { 0x3ADB9758U, 1224000 }, // sugoi
    { 0x3AF76F4AU, 38304000 }, // voltic2
    { 0x3AF8C345U, 38000 }, // sandking2
    { 0x3C26BD0CU, 12095000 }, // impaler2
    { 0x3C4E2113U, 665000 }, // coquette2
    { 0x3D29CD2BU, 195000 }, // youga2
    { 0x3D7C6410U, 2825000 }, // tezeract
    { 0x3D8FA25CU, 120000 }, // ninef
    { 0x3DA47243U, 1440000 }, // nero
    { 0x3DC92356U, 26533500 }, // nokota
    { 0x3DEE5EDAU, 42000 }, // blista2
    { 0x3E2E4F8AU, 51737000 }, // tula
    { 0x3E3D1F59U, 2325000 }, // thrax
    { 0x3E5BD8D9U, 1225000 }, // michelli
    { 0x3EAB5555U, 350000 }, // jb700
    { 0x3FC5D440U, 23000 }, // bobcatxl
    { 0x3FD5AA2FU, 1750000 }, // toro
    { 0x400F5147U, 252000 }, // specter2
    { 0x4019CB4CU, 5150000 }, // swift2
    { 0x403820E8U, 13233500 }, // velum2
    { 0x404B6381U, 400000 }, // pigalle
    { 0x40C332A3U, 225000 }, // manchez2
    { 0x4131F378U, 605000 }, // nero2
    { 0x41B77FA4U, 695000 }, // verlierer2
    { 0x41D149AAU, 650000 }, // sentinel3
    { 0x4201A843U, 620000 }, // peyote3
    { 0x42836BE5U, 830000 }, // hotring
    { 0x42ACA95FU, 408000 }, // asbo
    { 0x42BC5E19U, 415000 }, // slamvan3
    { 0x42F2ED16U, 250000 }, // superd
    { 0x432AA566U, 16000 }, // bfinjection
    { 0x4339CD69U, 10000 }, // tribike
    { 0x43779C54U, 8000 }, // bmx
    { 0x440851D8U, 1797000 }, // comet7
    { 0x4543B74DU, 13000 }, // rumpo
    { 0x4662BCBBU, 14896000 }, // technical2
    { 0x46699F47U, 37040500 }, // akula
    { 0x4669D038U, 2997000 }, // openwheel2
    { 0x47BBCF2EU, 253000 }, // xls
    { 0x48CECED3U, 30000 }, // seminole
    { 0x494752F7U, 1815000 }, // seasparrow2
    { 0x49863E9CU, 500000 }, // marshall
    { 0x4992196CU, 1260000 }, // gp1
    { 0x49E25BA1U, 1089000 }, // issi6
    { 0x4ABEBF23U, 1775000 }, // caracara
    { 0x4B6C568AU, 82000 }, // hakuchou
    { 0x4BA4E8DCU, 58000 }, // landstalker
    { 0x4BFCF28BU, 610000 }, // bestiagts
    { 0x4C3FFF49U, 512000 }, // deviant
    { 0x4C80EB0EU, 550000 }, // airbus
    { 0x4C8DBA51U, 2400000 }, // zhaba
    { 0x4DC079D7U, 1627000 }, // growler
    { 0x4EE74355U, 2750000 }, // emerus
    { 0x4FAF0D70U, 2200000 }, // kosatka
    { 0x4FB1A214U, 60000 }, // serrano
    { 0x4FF77E37U, 950000 }, // vestra
    { 0x506434F6U, 82000 }, // oracle
    { 0x50732C82U, 60000 }, // sentinel
    { 0x5097F589U, 1603000 }, // sc1
    { 0x50A6FB9CU, 24805000 }, // shinobi
    { 0x50D4D19FU, 1425000 }, // technical3
    { 0x51D83328U, 120000 }, // warrener
    { 0x5216AD5EU, 1370000 }, // remus
    { 0x52FF9437U, 1890000 }, // cyclone
    { 0x546DA331U, 1490000 }, // previon
    { 0x5502626CU, 1750000 }, // fmj
    { 0x55365079U, 610000 }, // brioso2
    { 0x56C8A5EFU, 3660000 }, // toreador
    { 0x56CDEE7DU, 1285000 }, // vstr
    { 0x56D42971U, 718000 }, // tulip
    { 0x57F682AFU, 130000 }, // rumpo3
    { 0x586765FBU, 47215000 }, // deluxo
    { 0x58B3979CU, 25000 }, // paradise
    { 0x58CDAF30U, 36575000 }, // thruster
    { 0x58CF185CU, 208000 }, // schafter4
    { 0x58E316C7U, 1995000 }, // sanctus
    { 0x58F77553U, 3400000 }, // openwheel1
    { 0x5993F939U, 1225000 }, // trailerlarge
    { 0x59A9E570U, 998000 }, // torero
    { 0x59E0FBF3U, 9000 }, // picador
    { 0x5B531351U, 1845000 }, // deity
    { 0x5BA0FF1EU, 1089000 }, // issi5
    { 0x5BEB3CE0U, 30762900 }, // scarab2
    { 0x5C23AF9BU, 850000 }, // stinger
    { 0x5C55CB39U, 155000 }, // brioso
    { 0x5D1903F9U, 710000 }, // comet4
    { 0x5D56F01BU, 4788000 }, // molotok
    { 0x5E4327C8U, 845000 }, // windsor
    { 0x5EE005DAU, 1795000 }, // deveste
    { 0x6068AD86U, 335000 }, // fagaloa
    { 0x619C1B82U, 22849400 }, // imperator2
    { 0x61FE4D6AU, 870000 }, // weevil
    { 0x6210CBB0U, 9000 }, // rancherxl
    { 0x6290F15BU, 3205300 }, // pounder2
    { 0x6322B39AU, 2200000 }, // t20
    { 0x63ABADE7U, 9000 }, // akuma
    { 0x64DE07A1U, 3800000 }, // strikeforce
    { 0x64F49967U, 1308000 }, // yosemite2
    { 0x665F785DU, 925000 }, // manana2
    { 0x669EB40AU, 15308750 }, // monster3
    { 0x66B4FC45U, 10000 }, // stratum
    { 0x67D2B389U, 500000 }, // streiter
    { 0x67D52852U, 13218750 }, // slamvan6
    { 0x6827CF72U, 2240000 }, // stockade
    { 0x6882FA73U, 48000 }, // enduro
    { 0x68A5D1EFU, 1550000 }, // cypher
    { 0x69F06B57U, 15000 }, // washington
    { 0x6ABDF65EU, 245000 }, // diablous2
    { 0x6B73A9BEU, 1288000 }, // youga3
    { 0x6CBD1D6DU, 1150000 }, // besra
    { 0x6D19CCBCU, 38000 }, // peyote
    { 0x6D6F8F43U, 75000 }, // thrust
    { 0x6DBD6C0AU, 615000 }, // retinue
    { 0x6E8DA4F7U, 897000 }, // issi7
    { 0x6EF89CCCU, 2125000 }, // longfin
    { 0x6F039A67U, 812000 }, // zion3
    { 0x6F946279U, 485000 }, // yosemite
    { 0x6FACDF31U, 48000 }, // ratbike
    { 0x6FF0F727U, 149000 }, // baller3
    { 0x706E2B40U, 599000 }, // specter
    { 0x707E63A4U, 816000 }, // tropos
    { 0x710A2B9BU, 370000 }, // moonbeam2
    { 0x711D4738U, 11305000 }, // dune3
    { 0x71CB2FFBU, 24000 }, // fugitive
    { 0x71CBEA98U, 940000 }, // gb200
    { 0x71D3B6F0U, 38703000 }, // cerberus3
    { 0x72934BE4U, 438000 }, // schafter6
    { 0x72A4C31EU, 71000 }, // stalion
    { 0x734C5E50U, 745000 }, // gauntlet4
    { 0x73920F8EU, 3295000 }, // firetruk
    { 0x7397224CU, 1535000 }, // vagner
    { 0x73F4110EU, 957600 }, // mule4
    { 0x761E2AD3U, 2000000 }, // titan
    { 0x767164D6U, 1950000 }, // osiris
    { 0x76D7C404U, 1900000 }, // reever
    { 0x779B4F2DU, 420000 }, // voodoo
    { 0x779F23AAU, 60000 }, // cavalcade
    { 0x780FFBD2U, 1630000 }, // vetir
    { 0x7836CE2FU, 9000 }, // futo
    { 0x79178F0AU, 1620000 }, // retinue2
    { 0x794CB30CU, 264000 }, // esskey
    { 0x7980BDD5U, 1800000 }, // euros
    { 0x798682A2U, 26666500 }, // brutus3
    { 0x79DD18AEU, 1775000 }, // menacer
    { 0x7A2EF5E4U, 885000 }, // rapidgt3
    { 0x7B406EFBU, 2550000 }, // tyrus
    { 0x7B47A6A7U, 650000 }, // lurcher
    { 0x7B54A9D3U, 38902500 }, // oppressor2
    { 0x7B7E56F0U, 8977500 }, // insurgent2
    { 0x7B8AB45FU, 195000 }, // carbonizzare
    { 0x7E8F677FU, 2700000 }, // prototipo
    { 0x7F3415E3U, 378000 }, // dukes3
    { 0x7F5C91F1U, 85000 }, // rocoto
    { 0x7F81A829U, 26666500 }, // brutus
    { 0x806B9CC3U, 16000 }, // bagger
    { 0x810369E2U, 1000000 }, // dump
    { 0x8125BCF9U, 8000 }, // blazer
    { 0x81634188U, 10000 }, // manana
    { 0x81794C70U, 250000 }, // stunt
    { 0x817AFAADU, 815000 }, // gauntlet5
    { 0x8198AEDCU, 2305000 }, // entity2
    { 0x81A9CDDFU, 36000 }, // faction
    { 0x81BD2ED0U, 3450000 }, // avenger
    { 0x81E38F7FU, 116000 }, // avarus
    { 0x825A9F4CU, 375000 }, // guardian
    { 0x829A3C44U, 1385000 }, // rallytruck
    { 0x82CAC433U, 1250000 }, // tug
    { 0x82E47E85U, 1280000 }, // club
    { 0x82E499FAU, 875000 }, // stingergt
    { 0x83051506U, 12635000 }, // technical
    { 0x83070B62U, 3318350 }, // impaler
    { 0x8408F33AU, 785000 }, // gt500
    { 0x84718D34U, 525000 }, // coach
    { 0x8526E2F5U, 13218750 }, // slamvan4
    { 0x85E8E76BU, 1189000 }, // italigtb
    { 0x8612B64BU, 22000 }, // rebel2
    { 0x8644331AU, 1609000 }, // bruiser3
    { 0x86618EDAU, 400000 }, // primo2
    { 0x866BCE26U, 695000 }, // faction3
    { 0x86FE0B60U, 254000 }, // cognoscenti
    { 0x877358ADU, 645000 }, // comet3
    { 0x885F3671U, 7315000 }, // pbus
    { 0x8911B9F5U, 145000 }, // feltzer2
    { 0x897AFC65U, 1375000 }, // terbyte
    { 0x89BA59F5U, 23009000 }, // havok
    { 0x8B13F083U, 30000 }, // stretch
    { 0x8B213907U, 3115000 }, // formula2
    { 0x8C2BD0DCU, 585000 }, // nightshade
    { 0x8CB29A14U, 132000 }, // rapidgt
    { 0x8CF5CAE1U, 900000 }, // windsor2
    { 0x8D45DF49U, 12095000 }, // impaler3
    { 0x8D4B7A8AU, 2025000 }, // insurgent3
    { 0x8E08EC82U, 6583500 }, // wastelander
    { 0x8E9254FBU, 26000 }, // asterope
    { 0x8F0E3594U, 38000 }, // surge
    { 0x8F49AE28U, 26666500 }, // brutus2
    { 0x8FB66F9BU, 10000 }, // premier
    { 0x8FD54EBBU, 1862000 }, // trailersmall2
    { 0x9114EADAU, 17955000 }, // insurgent
    { 0x91373058U, 1615000 }, // zr350
    { 0x91CA96EEU, 1500000 }, // neon
    { 0x920016F1U, 2295000 }, // volatus
    { 0x9229E4EBU, 475000 }, // faggio
    { 0x92EF6E04U, 1135000 }, // pfister811
    { 0x92F5024EU, 608000 }, // novak
    { 0x93F09558U, 1269000 }, // deathbike2
    { 0x94114926U, 678000 }, // seminole2
    { 0x94204D89U, 12000 }, // asea
    { 0x9472CD24U, 805000 }, // peyote2
    { 0x94B395C5U, 32000 }, // gauntlet
    { 0x94DA98EFU, 375000 }, // tornado5
    { 0x95466BDBU, 335000 }, // faction2
    { 0x9628879CU, 35000 }, // granger
    { 0x96E24857U, 665000 }, // microlight
    { 0x9734F3EAU, 880000 }, // penetrator
    { 0x97398A4BU, 695000 }, // seven70
    { 0x97553C28U, 1475000 }, // everon
    { 0x97E55D11U, 300000 }, // mammatus
    { 0x9804F4C7U, 12095000 }, // impaler4
    { 0x98F65A5EU, 1510000 }, // coquette4
    { 0x991EFC04U, 1878000 }, // comet6
    { 0x9A474B5EU, 1545000 }, // avisa
    { 0x9A9EB7DEU, 36575000 }, // starling
    { 0x9AE6DDA1U, 155000 }, // bullet
    { 0x9B065C9EU, 1609000 }, // bruiser2
    { 0x9B16A3B4U, 31255000 }, // riot2
    { 0x9B909C94U, 15000 }, // sabregt
    { 0x9C429B6AU, 450000 }, // velum
    { 0x9C5E5644U, 3330000 }, // supervolito2
    { 0x9C669788U, 12000 }, // double
    { 0x9CF21E0FU, 20000 }, // dune
    { 0x9CFFFC56U, 995000 }, // mamba
    { 0x9D0450CAU, 780000 }, // maverick
    { 0x9D96B45BU, 32000 }, // radi
    { 0x9DAE1398U, 25536000 }, // phantom2
    { 0x9F4B77BEU, 150000 }, // voltic
    { 0x9F6ED5A2U, 1875000 }, // neo
    { 0xA0438767U, 100000 }, // nightblade
    { 0xA09E15FDU, 37905000 }, // valkyrie
    { 0xA1355F67U, 17556000 }, // blazer5
    { 0xA1B3A871U, 1970000 }, // jester4
    { 0xA29D6D10U, 975000 }, // feltzer3
    { 0xA29F78B0U, 909000 }, // clique
    { 0xA31CB573U, 378000 }, // tornado6
    { 0xA3FC0F4DU, 29000 }, // gresley
    { 0xA42FC3A5U, 1785000 }, // vectre
    { 0xA4A4E453U, 380000 }, // riata
    { 0xA4D99B7DU, 1375000 }, // raiden
    { 0xA4F52C13U, 1740000 }, // cinquemila
    { 0xA52F6866U, 21213500 }, // alphaz1
    { 0xA5325278U, 67000 }, // manchez
    { 0xA6297CC8U, 1590000 }, // futo2
    { 0xA703E4A9U, 995000 }, // veto2
    { 0xA774B5A6U, 116000 }, // schafter3
    { 0xA7CE1BC5U, 715000 }, // brawler
    { 0xA7DCC35CU, 21386400 }, // zr3803
    { 0xA7EDE74DU, 10000 }, // stanier
    { 0xA8E38B01U, 130000 }, // ninef2
    { 0xA960B13EU, 8000 }, // sanchez2
    { 0xA988D3A2U, 25000 }, // prairie
    { 0xA9EC907BU, 2765000 }, // ignus
    { 0xAA699BB6U, 25000 }, // bodhi2
    { 0xAA6F980AU, 38503500 }, // khanjali
    { 0xAC33179CU, 915000 }, // infernus2
    { 0xAC4E93C9U, 145000 }, // daemon2
    { 0xAC5DF515U, 725000 }, // zentorno
    { 0xAD6065C0U, 44555000 }, // pyro
    { 0xAE0A3D4FU, 1132000 }, // dominator5
    { 0xAE12C99CU, 1269000 }, // deathbike3
    { 0xAE2BFE94U, 1263500 }, // kuruma
    { 0xAED64A63U, 180000 }, // chino2
    { 0xAF0B8D48U, 2310000 }, // tigon
    { 0xAF599F01U, 630000 }, // vindicator
    { 0xAF966F3CU, 875000 }, // caracara2
    { 0xB1D95DA0U, 650000 }, // cheetah
    { 0xB2A716A3U, 240000 }, // jester
    { 0xB2CF7250U, 1900000 }, // nimbus
    { 0xB2E046FBU, 1132000 }, // dominator6
    { 0xB2FE5CF9U, 795000 }, // entityxf
    { 0xB3206692U, 9000 }, // ingot
    { 0xB328B188U, 55000 }, // faggio3
    { 0xB39B0AE6U, 6500000 }, // lazer
    { 0xB44F0582U, 69000 }, // blazer3
    { 0xB472D2B5U, 565000 }, // ellie
    { 0xB4F32118U, 1675000 }, // flashgt
    { 0xB52B5113U, 65000 }, // schafter2
    { 0xB53C6C52U, 2275000 }, // minitank
    { 0xB5D306A4U, 1495000 }, // tailgater2
    { 0xB5EF4C33U, 3750000 }, // vigilante
    { 0xB6410173U, 249000 }, // dubsta3
    { 0xB67597ECU, 10000 }, // tribike2
    { 0xB6846A55U, 2475000 }, // le7b
    { 0xB779A091U, 1000000 }, // adder
    { 0xB79C1BF5U, 1150000 }, // shamal
    { 0xB79F589EU, 10000000 }, // luxor2
    { 0xB7D9F7F1U, 21080500 }, // tampa3
    { 0xB802DD46U, 3000 }, // rebel
    { 0xB820ED5EU, 160000 }, // blade
    { 0xB8D657ADU, 1995000 }, // calico
    { 0xB8E2AE18U, 65000 }, // zion2
    { 0xB9210FD0U, 45000 }, // sandking
    { 0xB9CB3B69U, 18000 }, // issi2
    { 0xBA5334ACU, 498000 }, // toros
    { 0xBB6B404FU, 9000 }, // primo
    { 0xBB78956AU, 3465000 }, // italirsx
    { 0xBBA2A2F7U, 30762900 }, // scarab
    { 0xBC32A33BU, 50000 }, // fq2
    { 0xBC5DC07EU, 1980000 }, // taipan
    { 0xBC7C0A00U, 2165000 }, // imorgon
    { 0xBC993509U, 25000 }, // dilettante
    { 0xBCDE91F0U, 330000 }, // minivan2
    { 0xBD1B39C3U, 60000 }, // zion
    { 0xBE0E6126U, 350000 }, // jester2
    { 0xBE11EFC6U, 21386400 }, // zr3802
    { 0xBE819C63U, 30000 }, // rentalbus
    { 0xBF1691E0U, 448000 }, // furoregt
    { 0xC0240885U, 995000 }, // tampa2
    { 0xC07107EEU, 1325000 }, // submersible2
    { 0xC1A8A914U, 1310000 }, // slamtruck
    { 0xC1AE4D16U, 100000 }, // comet2
    { 0xC1CE1183U, 4139900 }, // marquis
    { 0xC1E908D2U, 126000 }, // banshee
    { 0xC2974024U, 168990 }, // seashark
    { 0xC397F748U, 390000 }, // buccaneer2
    { 0xC3D7C72BU, 99000 }, // zombiea
    { 0xC3DDFDCEU, 55000 }, // tailgater
    { 0xC3F25753U, 12967500 }, // howard
    { 0xC4810400U, 2250000 }, // visione
    { 0xC514AAE0U, 145000 }, // cheburek
    { 0xC52C6B93U, 725000 }, // dominator3
    { 0xC575DF11U, 705000 }, // turismo2
    { 0xC58DA34AU, 1850000 }, // dinghy5
    { 0xC5DD6967U, 1596000 }, // rogue
    { 0xC7E55211U, 1625000 }, // locust
    { 0xC96B73D9U, 315000 }, // dominator2
    { 0xC972A155U, 2995000 }, // champion
    { 0xC98BBAD6U, 520000 }, // glendale2
    { 0xC9CEAF06U, 9000 }, // pcj
    { 0xC9E8FF76U, 5985000 }, // burrito2
    { 0xCA495705U, 500000 }, // dodo
    { 0xCA62927AU, 240000 }, // virgo2
    { 0xCABD11E8U, 9000 }, // ruffian
    { 0xCADD5D2DU, 15000 }, // bati2
    { 0xCB0E7CD9U, 325000 }, // schafter5
    { 0xCB642637U, 797000 }, // nebula
    { 0xCCE5C8FAU, 895000 }, // veto
    { 0xCD93A7DBU, 7420140 }, // monster
    { 0xCE0B9F22U, 1220000 }, // landstalker2
    { 0xCE44C4B9U, 1700000 }, // komoda
    { 0xCE6B35A4U, 550000 }, // btype2
    { 0xCEC6B9B7U, 21000 }, // vigero
    { 0xCEEA3F4BU, 450000 }, // barracks
    { 0xCFCFEB3BU, 50000 }, // patriot
    { 0xD039510BU, 38703000 }, // cerberus
    { 0xD1AD4937U, 701000 }, // omnis
    { 0xD2D5E00EU, 196000 }, // fcr2
    { 0xD2F77E37U, 22849400 }, // imperator3
    { 0xD35698EFU, 31255000 }, // mogul
    { 0xD37B7976U, 80000 }, // schwarzer
    { 0xD4AE63D9U, 1815000 }, // seasparrow
    { 0xD556917CU, 15308750 }, // monster5
    { 0xD577C962U, 500000 }, // bus
    { 0xD6BC7523U, 33117000 }, // chernobog
    { 0xD6FB0F30U, 1132000 }, // dominator4
    { 0xD756460CU, 29000 }, // buccaneer
    { 0xD757D97DU, 1925000 }, // zorrusso
    { 0xD7C56D39U, 648000 }, // raptor
    { 0xD80F4A44U, 1710000 }, // patriot3
    { 0xD83C13CEU, 6000 }, // ratloader
    { 0xD86A0247U, 2875000 }, // krieger
    { 0xD876DBE2U, 695000 }, // trophytruck2
    { 0xD9927FE3U, 240000 }, // cuban800
    { 0xD9F0503DU, 46284000 }, // scramjet
    { 0xDA288376U, 12000 }, // nemesis
    { 0xDA5819A3U, 385000 }, // massacro2
    { 0xDA5EC7DAU, 1380000 }, // penumbra2
    { 0xDAC67112U, 60000 }, // jackal
    { 0xDB0C9B04U, 2150000 }, // buffalo4
    { 0xDB20A373U, 95000 }, // wolfsbane
    { 0xDBA9DBFCU, 356000 }, // vortex
    { 0xDBF2D57AU, 558000 }, // cognoscenti2
    { 0xDC19D101U, 982000 }, // btype3
    { 0xDC434E51U, 35000 }, // sadler
    { 0xDCBCBE48U, 80000 }, // f620
    { 0xDCE1D9F7U, 375000 }, // ratloader2
    { 0xDD71BFEBU, 30762900 }, // scarab3
    { 0xDE05FB87U, 122000 }, // zombieb
    { 0xDE3D9D22U, 95000 }, // elegy2
    { 0xE18195B2U, 80000 }, // oracle2
    { 0xE1C03AB0U, 1300000 }, // schlagen
    { 0xE2504942U, 195000 }, // virgo
    { 0xE33A477BU, 495000 }, // italigtb2
    { 0xE505CF99U, 1715000 }, // rt3000
    { 0xE550775BU, 905000 }, // paragon
    { 0xE5BA6858U, 81000 }, // blazer4
    { 0xE62B361BU, 490000 }, // monroe
    { 0xE6401328U, 522000 }, // xls2
    { 0xE644E480U, 85000 }, // panto
    { 0xE6E967F8U, 6118000 }, // patriot2
    { 0xE78CC3D9U, 1610000 }, // revolter
    { 0xE7D2A16EU, 2225000 }, // shotaro
    { 0xE80F67EEU, 277000 }, // stalion2
    { 0xE823FB48U, 10000 }, // tribike3
    { 0xE8983F9FU, 11305000 }, // seabreeze
    { 0xE8A8BA94U, 875000 }, // viseris
    { 0xE8A8BDA8U, 90000 }, // felon
    { 0xE9805550U, 24000 }, // penumbra
    { 0xE99011C2U, 2515000 }, // tyrant
    { 0xEA313705U, 4350000 }, // alkonost
    { 0xEA6A047FU, 835000 }, // hellion
    { 0xEB298297U, 75000 }, // bifta
    { 0xEBC24DF2U, 1600000 }, // swift
    { 0xEC3E3404U, 1965000 }, // italigto
    { 0xEC8F7094U, 665000 }, // dukes2
    { 0xECA6B6A3U, 2575000 }, // s80
    { 0xED552C74U, 1955000 }, // autarch
    { 0xED62BFA9U, 3192000 }, // dune5
    { 0xED7EADA4U, 30000 }, // minivan
    { 0xEDA4ED97U, 11903500 }, // blimp3
    { 0xEDC6F847U, 1100000 }, // brickade
    { 0xEDD516C6U, 35000 }, // buffalo
    { 0xEE6024BCU, 795000 }, // sultanrs
    { 0xEEA75E63U, 1789000 }, // sultan3
    { 0xEEF345ECU, 1590000 }, // rcbandito
    { 0xEF2295C9U, 251600 }, // suntrap
    { 0xEF813606U, 2955000 }, // patrolboat
    { 0xF06C29C7U, 1380000 }, // granger2
    { 0xF0C2A91FU, 976000 }, // hakuchou2
    { 0xF1B44F44U, 169000 }, // diablous
    { 0xF26CEFF9U, 10000 }, // ruiner
    { 0xF330CB6AU, 790000 }, // jester3
    { 0xF34DFB25U, 21213500 }, // barrage
    { 0xF376F1E6U, 1100000 }, // winky
    { 0xF38C4245U, 1225000 }, // jugular
    { 0xF4E1AA15U, 2000 }, // scorcher
    { 0xF683EACAU, 925000 }, // innovation
    { 0xF77ADE32U, 275000 }, // massacro
    { 0xF79A00F7U, 9000 }, // vader
    { 0xF8C2E0E7U, 345000 }, // kamacho
    { 0xF8D48E7AU, 15000 }, // journey
    { 0xF92AEC4DU, 1650000 }, // limo2
    { 0xF9300CC5U, 15000 }, // bati
    { 0xF9E67C05U, 1130000 }, // squaddie
    { 0xFAAD85EEU, 95000 }, // felon2
    { 0xFB133A17U, 25935000 }, // savage
    { 0xFCC2F483U, 597000 }, // freecrawler
    { 0xFCFCB68BU, 1790000 }, // cargobob
    { 0xFD128DFDU, 596000 }, // vamos
    { 0xFD231729U, 62000 }, // blazer2
    { 0xFD707EDEU, 4123000 }, // hunter
    { 0xFE0A508CU, 59185000 }, // bombushka
    { 0xFE141DA6U, 22543500 }, // halftrack
    { 0xFE5F0722U, 1269000 }, // deathbike
    { 0xFEFD644FU, 30000 }, // bison
    { 0xFF22D208U, 8000 }, // regina
    { 0xFFB15B5EU, 205000 }, // exemplar
};

static int GetPhase3GtacarsPurchasePrice(
    Hash model)
{
    const uint32_t target =
        static_cast<uint32_t>(model);

    size_t low = 0;
    size_t high =
        sizeof(kPhase3GtacarsPrices)
        / sizeof(kPhase3GtacarsPrices[0]);

    while (low < high)
    {
        const size_t mid =
            low + (high - low) / 2;

        const uint32_t candidate =
            kPhase3GtacarsPrices[mid]
                .modelHash;

        if (candidate < target)
        {
            low = mid + 1;
        }
        else
        {
            high = mid;
        }
    }

    if (low
            < sizeof(kPhase3GtacarsPrices)
                / sizeof(kPhase3GtacarsPrices[0])
        && kPhase3GtacarsPrices[low]
            .modelHash == target)
    {
        return kPhase3GtacarsPrices[low]
            .purchasePrice;
    }

    return 0;
}

struct Phase3VehiclePriceEstimate
{
    int vehicleClass;
    int modelValue;
    int gtacarsPurchasePrice;
    int classMarketFloor;
    int baseMarketValue;
    int customizationRetailValue;
    int installedModCount;
    int performanceModCount;
    int toggleModCount;
    float bodyHealth;
    float engineHealth;
    int damagePercent;
    int damagePenaltyAmount;
    int dynamicSellPrice;
};

static const char* GetPhase3VehicleClassName(
    int vehicleClass)
{
    static const char* kNames[] =
    {
        "Compacts",
        "Sedans",
        "SUVs",
        "Coupes",
        "Muscle",
        "Sports Classics",
        "Sports",
        "Super",
        "Motorcycles",
        "Off-road",
        "Industrial",
        "Utility",
        "Vans",
        "Cycles",
        "Boats",
        "Helicopters",
        "Planes",
        "Service",
        "Emergency",
        "Military",
        "Commercial",
        "Trains",
        "Open Wheel"
    };

    if (vehicleClass < 0
        || vehicleClass
            >= static_cast<int>(
                sizeof(kNames)
                / sizeof(kNames[0])))
    {
        return "Unknown";
    }

    return kNames[vehicleClass];
}

static int GetPhase3ClassMarketFloor(
    int vehicleClass)
{
    // Replacement-value floors, not Sell prices. The configured resale
    // percentage is applied later to the corrected market value.
    static const int kFloors[] =
    {
        25000,    // Compacts
        35000,    // Sedans
        50000,    // SUVs
        65000,    // Coupes
        60000,    // Muscle
        175000,   // Sports Classics
        250000,   // Sports
        650000,   // Super
        45000,    // Motorcycles
        80000,    // Off-road
        120000,   // Industrial
        50000,    // Utility
        45000,    // Vans
        1500,     // Cycles
        175000,   // Boats
        650000,   // Helicopters
        750000,   // Planes
        60000,    // Service
        120000,   // Emergency
        450000,   // Military
        150000,   // Commercial
        250000,   // Trains
        1200000   // Open Wheel
    };

    if (vehicleClass < 0
        || vehicleClass
            >= static_cast<int>(
                sizeof(kFloors)
                / sizeof(kFloors[0])))
    {
        return 30000;
    }

    return kFloors[vehicleClass];
}

static int GetPhase3InstalledModRetailValue(
    int slot,
    int installedIndex,
    int availableCount)
{
    if (installedIndex < 0
        || availableCount <= 0)
    {
        return 0;
    }

    const int level = installedIndex + 1;

    switch (slot)
    {
    case 11: // Engine
        return 8000 + level * 7000;

    case 12: // Brakes
        return 7000 + level * 6500;

    case 13: // Transmission
        return 8000 + level * 7500;

    case 15: // Suspension
        return 5000 + level * 5000;

    case 16: // Armor
        return 15000 + level * 14000;

    case 23: // Front wheels
    case 24: // Rear wheels
        return 9000
            + (level > 20
                ? 20000
                : level * 1000);

    case 38: // Hydraulics
        return 30000 + level * 5000;

    case 39: // Engine block
    case 40: // Air filter
    case 41: // Struts
        return 8000 + level * 2500;

    default:
        break;
    }

    if (slot >= 0 && slot <= 10)
    {
        const int value =
            3500 + level * 1500;
        return value > 18000
            ? 18000
            : value;
    }

    if (slot == 14)
        return 3000 + level * 500;

    if (slot >= 25 && slot <= 48)
    {
        const int value =
            4000 + level * 1000;
        return value > 16000
            ? 16000
            : value;
    }

    return 3000 + level * 1000;
}

static int EstimatePhase3CustomizationRetailValue(
    Vehicle vehicle,
    int& installedModCount,
    int& performanceModCount,
    int& toggleModCount)
{
    installedModCount = 0;
    performanceModCount = 0;
    toggleModCount = 0;

    if (vehicle == 0
        || !ENTITY::DOES_ENTITY_EXIST(vehicle))
    {
        return 0;
    }

    int64_t total = 0;

    for (int slot = 0;
         slot <= 48;
         ++slot)
    {
        // Known toggle/unused slots are handled separately below.
        if (slot >= 17 && slot <= 22)
            continue;

        const int available =
            VEHICLE::GET_NUM_VEHICLE_MODS(
                vehicle,
                slot);

        if (available <= 0)
            continue;

        const int installed =
            VEHICLE::GET_VEHICLE_MOD(
                vehicle,
                slot);

        if (installed < 0)
            continue;

        ++installedModCount;

        if (slot == 11
            || slot == 12
            || slot == 13
            || slot == 15
            || slot == 16)
        {
            ++performanceModCount;
        }

        total +=
            GetPhase3InstalledModRetailValue(
                slot,
                installed,
                available);

        if ((slot == 23 || slot == 24)
            && VEHICLE::GET_VEHICLE_MOD_VARIATION(
                vehicle,
                slot))
        {
            total += 4000;
        }
    }

    if (VEHICLE::IS_TOGGLE_MOD_ON(vehicle, 18))
    {
        total += 50000; // Turbo
        ++toggleModCount;
        ++performanceModCount;
    }

    if (VEHICLE::IS_TOGGLE_MOD_ON(vehicle, 20))
    {
        total += 4000; // Tire smoke
        ++toggleModCount;
    }

    if (VEHICLE::IS_TOGGLE_MOD_ON(vehicle, 22))
    {
        total += 7500; // Xenon lights
        ++toggleModCount;
    }

    if (total > 1000000LL)
        total = 1000000LL;

    return static_cast<int>(total);
}

static bool EstimatePhase3VehicleSellPrice(
    Vehicle vehicle,
    Hash model,
    Phase3VehiclePriceEstimate& estimate)
{
    estimate = Phase3VehiclePriceEstimate{};

    if (vehicle == 0
        || !ENTITY::DOES_ENTITY_EXIST(vehicle))
    {
        return false;
    }

    estimate.vehicleClass =
        VEHICLE::GET_VEHICLE_CLASS(vehicle);

    estimate.modelValue =
        GetPhase3VehicleModelValue(model);

    estimate.gtacarsPurchasePrice =
        GetPhase3GtacarsPurchasePrice(
            model);

    estimate.classMarketFloor =
        GetPhase3ClassMarketFloor(
            estimate.vehicleClass);

    if (estimate.gtacarsPurchasePrice > 0)
    {
        // For known native vehicles, GTACars acquisition pricing is the
        // authoritative stock-value basis. Do not let nMonetaryValue drag
        // expensive Online-era vehicles down to traffic-car prices.
        estimate.baseMarketValue =
            estimate.gtacarsPurchasePrice;
    }
    else
    {
        estimate.baseMarketValue =
            estimate.modelValue
                > estimate.classMarketFloor
            ? estimate.modelValue
            : estimate.classMarketFloor;
    }

    estimate.customizationRetailValue =
        EstimatePhase3CustomizationRetailValue(
            vehicle,
            estimate.installedModCount,
            estimate.performanceModCount,
            estimate.toggleModCount);

    // Configurable resale estimate:
    // VehicleSellPercent of corrected stock value plus UpgradePercent of the
    // estimated retail value of installed upgrades.
    int64_t sellPrice =
        static_cast<int64_t>(
            estimate.baseMarketValue)
            * static_cast<int64_t>(
                g_vehicleSellPercent)
            / 100LL;

    sellPrice +=
        static_cast<int64_t>(
            estimate.customizationRetailValue)
            * static_cast<int64_t>(
                g_upgradePercent)
            / 100LL;

    estimate.bodyHealth =
        VEHICLE::GET_VEHICLE_BODY_HEALTH(
            vehicle);
    estimate.engineHealth =
        VEHICLE::GET_VEHICLE_ENGINE_HEALTH(
            vehicle);
    estimate.damagePercent = 0;
    estimate.damagePenaltyAmount = 0;

    if (g_useDamagePenalty)
    {
        float bodyHealth =
            estimate.bodyHealth;
        float engineHealth =
            estimate.engineHealth;

        if (bodyHealth < 0.0f)
            bodyHealth = 0.0f;
        else if (bodyHealth > 1000.0f)
            bodyHealth = 1000.0f;

        if (engineHealth < 0.0f)
            engineHealth = 0.0f;
        else if (engineHealth > 1000.0f)
            engineHealth = 1000.0f;

        // Use the worse of body/engine condition. A vehicle that is 20%
        // damaged therefore loses 20% of its calculated resale value.
        const float conditionHealth =
            bodyHealth < engineHealth
                ? bodyHealth
                : engineHealth;

        int conditionPermille =
            static_cast<int>(
                conditionHealth + 0.5f);

        if (conditionPermille < 0)
            conditionPermille = 0;
        else if (conditionPermille > 1000)
            conditionPermille = 1000;

        const int64_t beforeDamage =
            sellPrice;

        sellPrice =
            sellPrice
            * static_cast<int64_t>(
                conditionPermille)
            / 1000LL;

        estimate.damagePenaltyAmount =
            static_cast<int>(
                beforeDamage - sellPrice);

        estimate.damagePercent =
            (1000 - conditionPermille + 5)
            / 10;
    }

    if (sellPrice <= 0)
        return false;

    if (sellPrice > 2000000000LL)
        sellPrice = 2000000000LL;

    estimate.dynamicSellPrice =
        static_cast<int>(sellPrice);

    return true;
}

static void ResetPhase3PreparedSellPrice()
{
    g_phase3PreparedVehicle = 0;
    g_phase3PreparedModel = 0;
    g_phase3PreparedMenuState = -999;
    g_phase3PreparedSellPrice = 0;
    g_phase3PreparedSellPriceValid = false;
}

static void UpdatePhase3PreparedSellPrice()
{
    if (g_phase3CurrentMenuState < 0
        || g_phase3SellStageActive)
    {
        return;
    }

    const Ped playerPed =
        PLAYER::PLAYER_PED_ID();

    if (!PED::IS_PED_IN_ANY_VEHICLE(
            playerPed,
            false))
    {
        ResetPhase3PreparedSellPrice();
        return;
    }

    const Vehicle vehicle =
        PED::GET_VEHICLE_PED_IS_IN(
            playerPed,
            false);

    if (vehicle == 0
        || !ENTITY::DOES_ENTITY_EXIST(vehicle))
    {
        ResetPhase3PreparedSellPrice();
        return;
    }

    const Hash model =
        ENTITY::GET_ENTITY_MODEL(vehicle);

    // Recalculate only when the vehicle changes or the player changes LSC
    // menus. Returning from an upgrade category to the main menu therefore
    // picks up newly installed modifications without repeatedly scanning all
    // mod slots while the player simply sits on one menu.
    if (g_phase3PreparedSellPriceValid
        && vehicle == g_phase3PreparedVehicle
        && model == g_phase3PreparedModel
        && g_phase3CurrentMenuState
            == g_phase3PreparedMenuState)
    {
        return;
    }

    Phase3VehiclePriceEstimate estimate{};
    if (!EstimatePhase3VehicleSellPrice(
            vehicle,
            model,
            estimate))
    {
        ResetPhase3PreparedSellPrice();
        return;
    }

    g_phase3PreparedVehicle = vehicle;
    g_phase3PreparedModel = model;
    g_phase3PreparedMenuState =
        g_phase3CurrentMenuState;
    g_phase3PreparedSellPrice =
        estimate.dynamicSellPrice;
    g_phase3PreparedSellPriceValid =
        g_phase3PreparedSellPrice > 0;
}

static void FlushPhase3DisplayPriceEvent()
{
    if (!g_phase3DisplayPriceEventPending)
        return;

    g_phase3DisplayPriceEventPending = false;

    Logf(
        "[Phase3N] SellPriceDisplay injected=yes rockstarInitial=%d displayed=%d preparedDynamic=%d registerSite=0x%X nativeIndex=%u",
        g_phase3DisplayPriceOriginal,
        g_phase3DisplayPriceInjected,
        g_phase3PreparedSellPrice,
        g_phase3SellPricePath.registerNativeSite,
        static_cast<unsigned int>(
            g_phase3SellPricePath.registerNativeIndex));

    FlushLogBuffer();
}

static void UpdatePhase3SellPriceFallback()
{
    if (!g_phase3Enabled
        || !g_phase3SellPricePath.resolved
        || !g_carmodShopActive
        || g_lastNetworkGame)
    {
        g_phase3SellContextActive = false;
        g_phase3SellContextPrice = 0;
        g_phase3SellControlState = -1;
        g_phase3SellStageActive = false;

        if (!g_carmodShopActive)
        {
            g_phase3PriceThreadInfo = Phase2ThreadInfo{};
            g_phase3PriceThreadCached = false;
            g_nextPhase3PriceUpdateAt = 0;
            ResetPhase3PreparedSellPrice();
            g_phase3DisplayPriceEventPending = false;
        }

        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (now < g_nextPhase3PriceUpdateAt)
        return;

    g_nextPhase3PriceUpdateAt =
        now + kPhase3PriceUpdateIntervalMs;

    g_phase3SellContextActive = false;
    g_phase3SellContextPrice = 0;

    if (!g_phase3PriceThreadCached)
    {
        Phase2ThreadInfo threadInfo{};
        if (!GetPhase2ThreadInfo(
                kCarmodShopHash,
                threadInfo)
            || !threadInfo.stack)
        {
            return;
        }

        g_phase3PriceThreadInfo = threadInfo;
        g_phase3PriceThreadCached = true;
    }

    UpdatePhase3SellControlStateFast();
    UpdatePhase3PreparedSellPrice();
    FlushPhase3DisplayPriceEvent();

    if (!g_phase3SellStageActive)
        return;

    const uint32_t index =
        g_phase3SellPricePath.element0StaticIndex;

    if (index >= g_phase3PriceThreadInfo.stackSize)
    {
        g_phase3PriceThreadInfo = Phase2ThreadInfo{};
        g_phase3PriceThreadCached = false;
        return;
    }

    unsigned char* slot =
        reinterpret_cast<unsigned char*>(
            g_phase3PriceThreadInfo.stack)
        + static_cast<size_t>(index)
            * sizeof(uintptr_t);

    uint64_t raw = 0;
    if (!IsReadableMemory(slot, sizeof(raw)))
    {
        g_phase3PriceThreadInfo = Phase2ThreadInfo{};
        g_phase3PriceThreadCached = false;
        return;
    }

    std::memcpy(&raw, slot, sizeof(raw));

    const int32_t rockstarPrice =
        static_cast<int32_t>(
            raw & 0xFFFFFFFFULL);

    const Ped playerPed =
        PLAYER::PLAYER_PED_ID();

    if (!PED::IS_PED_IN_ANY_VEHICLE(
            playerPed,
            false))
    {
        return;
    }

    const Vehicle vehicle =
        PED::GET_VEHICLE_PED_IS_IN(
            playerPed,
            false);

    if (vehicle == 0
        || !ENTITY::DOES_ENTITY_EXIST(vehicle))
    {
        return;
    }

    const Hash model =
        ENTITY::GET_ENTITY_MODEL(vehicle);

    if (vehicle != g_phase3FallbackVehicle
        || model != g_phase3FallbackModel)
    {
        g_phase3FallbackVehicle = vehicle;
        g_phase3FallbackModel = model;
        g_phase3FallbackPrice = 0;
        g_phase3FallbackLogged = false;
    }

    if (g_phase3FallbackPrice <= 0)
    {
        Phase3VehiclePriceEstimate estimate{};

        if (!EstimatePhase3VehicleSellPrice(
                vehicle,
                model,
                estimate))
        {
            if (!g_phase3FallbackLogged)
            {
                Logf(
                    "[Phase3N] SellPriceDynamic=no vehicle=%d model=0x%08X rockstarPrice=%d reason=estimate unavailable",
                    static_cast<int>(vehicle),
                    static_cast<unsigned int>(model),
                    static_cast<int>(rockstarPrice));
                g_phase3FallbackLogged = true;
            }

            if (rockstarPrice > 0)
            {
                g_phase3SellContextActive = true;
                g_phase3SellContextPrice =
                    static_cast<int>(rockstarPrice);
            }

            return;
        }

        const int finalPrice =
            estimate.dynamicSellPrice;

        g_phase3FallbackPrice =
            finalPrice;

        Logf(
            "[Phase3N] SellPriceDynamic=yes vehicle=%d model=0x%08X class=%d(%s) rockstarPrice=%d modelValue=%d gtacarsPurchase=%d classFloor=%d baseMarket=%d vehicleSellPercent=%d customizationRetail=%d upgradePercent=%d installedMods=%d performanceMods=%d toggleMods=%d damagePenalty=%s bodyHealth=%.1f engineHealth=%.1f damagePercent=%d damagePenaltyAmount=%d dynamicSell=%d final=%d source=%s",
            static_cast<int>(vehicle),
            static_cast<unsigned int>(model),
            estimate.vehicleClass,
            GetPhase3VehicleClassName(
                estimate.vehicleClass),
            static_cast<int>(rockstarPrice),
            estimate.modelValue,
            estimate.gtacarsPurchasePrice,
            estimate.classMarketFloor,
            estimate.baseMarketValue,
            g_vehicleSellPercent,
            estimate.customizationRetailValue,
            g_upgradePercent,
            estimate.installedModCount,
            estimate.performanceModCount,
            estimate.toggleModCount,
            g_useDamagePenalty ? "on" : "off",
            estimate.bodyHealth,
            estimate.engineHealth,
            estimate.damagePercent,
            estimate.damagePenaltyAmount,
            estimate.dynamicSellPrice,
            finalPrice,
            estimate.gtacarsPurchasePrice > 0
                ? "gtacars"
                : "dynamic-fallback");

        g_phase3FallbackLogged = true;
    }

    const int finalPrice =
        g_phase3FallbackPrice;

    if (finalPrice <= 0)
        return;

    if (rockstarPrice != finalPrice)
    {
        const uint64_t patchedRaw =
            (raw & 0xFFFFFFFF00000000ULL)
            | static_cast<uint32_t>(
                finalPrice);

        std::memcpy(
            slot,
            &patchedRaw,
            sizeof(patchedRaw));

        uint64_t verify = 0;
        std::memcpy(
            &verify,
            slot,
            sizeof(verify));

        const int32_t verifiedPrice =
            static_cast<int32_t>(
                verify & 0xFFFFFFFFULL);

        if (verifiedPrice != finalPrice)
        {
            Logf(
                "[Phase3N] SellPriceDynamic write failed vehicle=%d model=0x%08X requested=%d observed=%d",
                static_cast<int>(vehicle),
                static_cast<unsigned int>(model),
                finalPrice,
                static_cast<int>(verifiedPrice));
            return;
        }
    }

    g_phase3SellContextActive = true;
    g_phase3SellContextPrice =
        finalPrice;
}

static void UpdatePhase3SellControlStateFast()
{
    g_phase3SellStageActive = false;

    if (!g_stockLscScopeActive
        || !g_phase3Enabled
        || !g_carmodShopActive
        || g_lastNetworkGame
        || !g_phase3SellStagePath.resolved
        || !g_phase3SellControlPath.resolved
        || !g_phase3PriceThreadCached
        || !g_phase3PriceThreadInfo.stack)
    {
        g_phase3CurrentMenuState = -1;
        g_phase3SellControlState = -1;
        return;
    }

    const uint32_t menuIndex =
        g_phase3SellStagePath.staticIndex;
    const uint32_t controlIndex =
        g_phase3SellControlPath.staticIndex;

    if (menuIndex >= g_phase3PriceThreadInfo.stackSize
        || controlIndex
            >= g_phase3PriceThreadInfo.stackSize)
    {
        g_phase3CurrentMenuState = -1;
        g_phase3SellControlState = -1;
        g_phase3PriceThreadInfo = Phase2ThreadInfo{};
        g_phase3PriceThreadCached = false;
        return;
    }

    const unsigned char* stack =
        reinterpret_cast<const unsigned char*>(
            g_phase3PriceThreadInfo.stack);

    const unsigned char* menuSlot =
        stack
        + static_cast<size_t>(menuIndex)
            * sizeof(uintptr_t);

    uint64_t menuRaw = 0;
    std::memcpy(
        &menuRaw,
        menuSlot,
        sizeof(menuRaw));

    g_phase3CurrentMenuState =
        static_cast<int32_t>(
            menuRaw & 0xFFFFFFFFULL);

    g_phase3SellStageActive =
        g_phase3CurrentMenuState
        == g_phase3SellStagePath.sellMenuValue;

    if (g_phase3CurrentMenuState
            != g_phase3LastLoggedMenuState)
    {
        const bool crossedSellBoundary =
            g_phase3CurrentMenuState
                == g_phase3SellStagePath.sellMenuValue
            || g_phase3LastLoggedMenuState
                == g_phase3SellStagePath.sellMenuValue;

        if (crossedSellBoundary)
        {
            Logf(
                "[Phase3P] SellStage currentMenu=%d sellMenu=%d active=%s staticIndex=%u raw=0x%016llX",
                g_phase3CurrentMenuState,
                g_phase3SellStagePath.sellMenuValue,
                g_phase3SellStageActive
                    ? "yes"
                    : "no",
                static_cast<unsigned int>(
                    menuIndex),
                static_cast<unsigned long long>(
                    menuRaw));

            if (!g_phase3SellStageActive)
            {
                // Re-estimate on the next Sell entry so upgrades installed
                // during this LSC visit are reflected in resale value.
                g_phase3FallbackVehicle = 0;
                g_phase3FallbackModel = 0;
                g_phase3FallbackPrice = 0;
                g_phase3FallbackLogged = false;
                g_phase3PreparedMenuState = -999;
            }

            FlushLogBuffer();
        }

        g_phase3LastLoggedMenuState =
            g_phase3CurrentMenuState;
    }

    if (!g_phase3SellStageActive)
    {
        g_phase3SellControlState = -1;

        if (g_phase3LastLoggedSellControlState != -1)
        {
            Logf(
                "[Phase3P] SellControl state=-1 reason=not-in-sell-stage");
            g_phase3LastLoggedSellControlState = -1;
            FlushLogBuffer();
        }

        return;
    }

    const unsigned char* controlSlot =
        stack
        + static_cast<size_t>(controlIndex)
            * sizeof(uintptr_t);

    uint64_t controlRaw = 0;
    std::memcpy(
        &controlRaw,
        controlSlot,
        sizeof(controlRaw));

    const int value =
        static_cast<int32_t>(
            controlRaw & 0xFFFFFFFFULL);

    g_phase3SellControlState =
        value >= 0 && value <= 3
            ? value
            : -1;

    if (g_phase3SellControlState
            != g_phase3LastLoggedSellControlState)
    {
        Logf(
            "[Phase3P] SellControl state=%d staticIndex=%u raw=0x%016llX",
            g_phase3SellControlState,
            static_cast<unsigned int>(
                controlIndex),
            static_cast<unsigned long long>(
                controlRaw));

        g_phase3LastLoggedSellControlState =
            g_phase3SellControlState;

        FlushLogBuffer();
    }
}

static void UpdateSellCompletionController()
{
    SellCompletion::Update(
        g_carmodShopActive,
        g_phase3SellContextActive,
        g_phase3SellContextPrice,
        g_phase3SellControlState);
}

static void ResetPhase3NativeProbeState()
{
    for (size_t i = 0; i < kPhase3NativeProbeCount; ++i)
    {
        g_phase3NativeProbes[i].nativeIndex = 0xFFFF;
        g_phase3NativeProbes[i].site = 0;
        g_phase3NativeProbes[i].expectedArgs = 0;
        g_phase3NativeProbes[i].expectedReturns = 0;
        g_phase3NativeProbes[i].original = nullptr;
        g_phase3NativeProbes[i].installed = false;
        g_phase3NativeProbes[i].calls = 0;
        g_phase3NativeProbes[i].matchedCalls = 0;
        g_phase3NativeProbes[i].unmatchedInSellLogged = 0;
        g_phase3NativeProbes[i].hasLoggedState = false;
        g_phase3NativeProbes[i].lastLoggedSite = 0;
        g_phase3NativeProbes[i].lastLoggedContextArgCount = 0;
        g_phase3NativeProbes[i].lastLoggedArg0 = 0;
        g_phase3NativeProbes[i].lastLoggedArg1 = 0;
        g_phase3NativeProbes[i].lastLoggedReturnRaw = 0;
        g_phase3NativeProbes[i].lastLoggedFrameLocal2 = 0;
        g_phase3NativeProbes[i].lastLoggedReturnReadable = false;
        g_phase3NativeProbes[i].lastLoggedFrameLocal2Readable = false;
        g_phase3NativeProbes[i].suppressedDuplicateLogs = 0;
    }

    g_phase3LaterStateGateASite = 0;
    g_phase3LaterStateGateBSite = 0;
    g_phase3SecondaryStateTestSite = 0;

    for (size_t i = 0; i < sizeof(g_phase3BitUpdateSites) / sizeof(g_phase3BitUpdateSites[0]); ++i)
        g_phase3BitUpdateSites[i] = 0;

    for (size_t i = 0; i < sizeof(g_phase3DelaySites) / sizeof(g_phase3DelaySites[0]); ++i)
        g_phase3DelaySites[i] = 0;

    for (size_t i = 0; i < sizeof(g_phase3HelperGateSites) / sizeof(g_phase3HelperGateSites[0]); ++i)
        g_phase3HelperGateSites[i] = Phase3HelperGateSite{};

    g_phase3HelperGateSiteCount = 0;
    g_phase3HelperNetworkOriginal = nullptr;
    g_phase3HelperNetworkInstalled = false;
    g_phase3HelperTraceThread = nullptr;
    g_phase3HelperTraceUntil = 0;
    g_phase3HelperTraceSequence = 0;
    g_phase3NativeProbeProgram = nullptr;
}

static bool WritePhase3NativeHandlerSlot(
    Phase2ScrProgram* program,
    uint16_t nativeIndex,
    Phase3NativeHandler handler)
{
    if (!program
        || !handler
        || program->nativeCount <= 0
        || nativeIndex >= static_cast<uint16_t>(program->nativeCount)
        || !program->nativeOffset)
    {
        return false;
    }

    Phase3NativeHandler* table =
        reinterpret_cast<Phase3NativeHandler*>(program->nativeOffset);

    if (!IsReadableMemory(
            table,
            sizeof(Phase3NativeHandler)
                * static_cast<size_t>(program->nativeCount)))
    {
        return false;
    }

    Phase3NativeHandler* slot = &table[nativeIndex];

    DWORD oldProtection = 0;
    if (!VirtualProtect(
            slot,
            sizeof(Phase3NativeHandler),
            PAGE_EXECUTE_READWRITE,
            &oldProtection))
    {
        Logf(
            "[Phase3Probe] VirtualProtect failed nativeIndex=%u error=%lu",
            static_cast<unsigned int>(nativeIndex),
            static_cast<unsigned long>(GetLastError()));
        return false;
    }

    *slot = handler;

    DWORD ignored = 0;
    VirtualProtect(
        slot,
        sizeof(Phase3NativeHandler),
        oldProtection,
        &ignored);

    return *slot == handler;
}

static void Phase3SellDisplayPriceRegisterHook(
    Phase3NativeCallContext* context)
{
    Phase3NativeHandler original =
        g_phase3SellDisplayPriceHook.original;

    if (!original
        || original
            == &Phase3SellDisplayPriceRegisterHook)
    {
        return;
    }

    // SECURITY::REGISTER_SCRIPT_VARIABLE receives INT*. Only modify the call
    // whose pointer is exactly the structurally resolved iOptionCost[0] stack
    // slot. Every other registration is a pure pass-through.
    if (g_stockLscScopeActive
        && g_carmodShopActive
        && !g_lastNetworkGame
        && g_phase3PreparedSellPriceValid
        && g_phase3PreparedSellPrice > 0
        && g_phase3PriceThreadCached
        && g_phase3PriceThreadInfo.stack
        && context
        && context->args
        && context->argCount >= 1
        && g_phase3SellPricePath.resolved
        && g_phase3SellPricePath.element0StaticIndex
            < g_phase3PriceThreadInfo.stackSize)
    {
        unsigned char* expectedSlot =
            reinterpret_cast<unsigned char*>(
                g_phase3PriceThreadInfo.stack)
            + static_cast<size_t>(
                g_phase3SellPricePath
                    .element0StaticIndex)
                * sizeof(uintptr_t);

        const uint64_t* rawArgs =
            reinterpret_cast<const uint64_t*>(
                context->args);

        const uintptr_t registeredPointer =
            static_cast<uintptr_t>(
                rawArgs[0]);

        if (registeredPointer
            == reinterpret_cast<uintptr_t>(
                expectedSlot))
        {
            int32_t rockstarInitial = 0;
            std::memcpy(
                &rockstarInitial,
                expectedSlot,
                sizeof(rockstarInitial));

            const int correctedPrice =
                g_phase3PreparedSellPrice;

            if (correctedPrice > 0
                && correctedPrice
                    != rockstarInitial)
            {
                const int32_t corrected =
                    static_cast<int32_t>(
                        correctedPrice);

                // Write before calling Rockstar's security native so its
                // protected shadow copy records the corrected value too.
                std::memcpy(
                    expectedSlot,
                    &corrected,
                    sizeof(corrected));
            }

            g_phase3DisplayPriceOriginal =
                static_cast<int>(
                    rockstarInitial);
            g_phase3DisplayPriceInjected =
                correctedPrice;
            g_phase3DisplayPriceEventPending =
                true;
        }
    }

    original(context);
}

static bool InstallPhase3SellDisplayPriceHook(
    Phase2ScrProgram* program)
{
    if (!program
        || !g_phase3SellPricePath.resolved
        || g_phase3SellPricePath.registerNativeIndex
            == 0xFFFF
        || g_phase3SellPricePath.registerNativeSite == 0
        || program->nativeCount <= 0
        || !program->nativeOffset
        || g_phase3SellPricePath.registerNativeIndex
            >= static_cast<uint16_t>(
                program->nativeCount))
    {
        return false;
    }

    if (g_phase3SellDisplayPriceHook.installed
        && g_phase3SellDisplayPriceHook.program
            == program
        && g_phase3SellDisplayPriceHook.nativeIndex
            == g_phase3SellPricePath.registerNativeIndex)
    {
        return true;
    }

    Phase3NativeHandler* table =
        reinterpret_cast<Phase3NativeHandler*>(
            program->nativeOffset);

    if (!IsReadableMemory(
            table,
            sizeof(Phase3NativeHandler)
                * static_cast<size_t>(
                    program->nativeCount)))
    {
        return false;
    }

    const uint16_t nativeIndex =
        g_phase3SellPricePath.registerNativeIndex;

    Phase3NativeHandler original =
        table[nativeIndex];

    if (!original
        || original
            == &Phase3SellDisplayPriceRegisterHook)
    {
        return false;
    }

    g_phase3SellDisplayPriceHook.program =
        program;
    g_phase3SellDisplayPriceHook.nativeSite =
        g_phase3SellPricePath.registerNativeSite;
    g_phase3SellDisplayPriceHook.nativeIndex =
        nativeIndex;
    g_phase3SellDisplayPriceHook.original =
        original;
    g_phase3SellDisplayPriceHook.installed =
        false;

    if (!WritePhase3NativeHandlerSlot(
            program,
            nativeIndex,
            &Phase3SellDisplayPriceRegisterHook))
    {
        g_phase3SellDisplayPriceHook =
            Phase3SellDisplayPriceHook{};
        return false;
    }

    g_phase3SellDisplayPriceHook.installed =
        true;

    Logf(
        "[Phase3N] SellPriceDisplayHook=yes registerSite=0x%X nativeIndex=%u scope=exact-iOptionCost0-pointer",
        g_phase3SellDisplayPriceHook.nativeSite,
        static_cast<unsigned int>(
            nativeIndex));

    return true;
}

static void Phase3SellCooldownClockHook(
    Phase3NativeCallContext* context)
{
    Phase3NativeHandler original =
        g_phase3SellCooldownHook.original;

    if (!original
        || original
            == &Phase3SellCooldownClockHook)
    {
        return;
    }

    original(context);

    if (!g_stockLscScopeActive
        || !g_carmodShopActive
        || g_lastNetworkGame
        || !g_phase3SellCooldownHook.installed
        || !g_phase3SellCooldownPath.resolved
        || !g_phase3PriceThreadCached
        || !g_phase3PriceThreadInfo.thread
        || !context
        || !context->returnValue
        || !IsReadableMemory(
            context->returnValue,
            sizeof(uint64_t)))
    {
        return;
    }

    uint32_t programCounter = 0;
    const size_t pcOffset =
        IsEnhancedEdition()
            ? 0x1C
            : 0x14;

    if (!ReadMemoryValue(
            g_phase3PriceThreadInfo.thread,
            pcOffset,
            programCounter)
        || !IsPhase3PcAtNativeSite(
            programCounter,
            g_phase3SellCooldownHook.nativeSite))
    {
        return;
    }

    int clockValue = 2147483647;
    int remainingSeconds = 0;

    if (g_useSellCooldown)
    {
        int activeClockValue = 0;
        int activeRemainingSeconds = 0;

        if (SellCompletion::
                TryGetCooldownClockOverride(
                    activeClockValue,
                    activeRemainingSeconds))
        {
            clockValue =
                activeClockValue;
            remainingSeconds =
                activeRemainingSeconds;
        }
    }

    const uint64_t rawValue =
        static_cast<uint64_t>(
            static_cast<uint32_t>(
                clockValue));

    std::memcpy(
        context->returnValue,
        &rawValue,
        sizeof(rawValue));

    g_phase3CooldownGateClockValue =
        clockValue;
    g_phase3CooldownGateRemainingSeconds =
        remainingSeconds;
    g_phase3CooldownGateEventPending =
        true;
}

static bool InstallPhase3SellCooldownHook(
    Phase2ScrProgram* program)
{
    if (!program
        || !g_phase3SellCooldownPath.resolved
        || g_phase3SellCooldownPath.clockNativeIndex
            == 0xFFFF
        || g_phase3SellCooldownPath.clockNativeSite
            == 0
        || program->nativeCount <= 0
        || !program->nativeOffset
        || g_phase3SellCooldownPath.clockNativeIndex
            >= static_cast<uint16_t>(
                program->nativeCount))
    {
        return false;
    }

    if (g_phase3SellCooldownHook.installed
        && g_phase3SellCooldownHook.program
            == program
        && g_phase3SellCooldownHook.nativeIndex
            == g_phase3SellCooldownPath
                .clockNativeIndex)
    {
        return true;
    }

    Phase3NativeHandler* table =
        reinterpret_cast<Phase3NativeHandler*>(
            program->nativeOffset);

    if (!IsReadableMemory(
            table,
            sizeof(Phase3NativeHandler)
                * static_cast<size_t>(
                    program->nativeCount)))
    {
        return false;
    }

    const uint16_t nativeIndex =
        g_phase3SellCooldownPath
            .clockNativeIndex;

    Phase3NativeHandler original =
        table[nativeIndex];

    if (!original
        || original
            == &Phase3SellCooldownClockHook)
    {
        return false;
    }

    g_phase3SellCooldownHook.program =
        program;
    g_phase3SellCooldownHook.nativeSite =
        g_phase3SellCooldownPath
            .clockNativeSite;
    g_phase3SellCooldownHook.nativeIndex =
        nativeIndex;
    g_phase3SellCooldownHook.original =
        original;
    g_phase3SellCooldownHook.installed =
        false;

    if (!WritePhase3NativeHandlerSlot(
            program,
            nativeIndex,
            &Phase3SellCooldownClockHook))
    {
        g_phase3SellCooldownHook =
            Phase3SellCooldownHook{};
        return false;
    }

    g_phase3SellCooldownHook.installed =
        true;

    Logf(
        "[Gameplay] SellCooldownHook=yes clockSite=0x%X nativeIndex=%u scope=exact-CMOD_NOSELL3-clock-call",
        g_phase3SellCooldownHook.nativeSite,
        static_cast<unsigned int>(
            nativeIndex));

    return true;
}

static void FlushPhase3SellCooldownGateEvent()
{
    if (!g_phase3CooldownGateEventPending)
        return;

    g_phase3CooldownGateEventPending =
        false;

    const int remaining =
        g_phase3CooldownGateRemainingSeconds;

    const bool shouldLog =
        g_phase3CooldownLastLoggedRemainingSeconds
            < 0
        || remaining == 0
        || remaining
            <= g_phase3CooldownLastLoggedRemainingSeconds
                - 30;

    if (!shouldLog)
        return;

    g_phase3CooldownLastLoggedRemainingSeconds =
        remaining;

    Logf(
        "[Gameplay] SellCooldownGate active=%s remainingSeconds=%d clockOverride=%d",
        remaining > 0 ? "yes" : "no",
        remaining,
        g_phase3CooldownGateClockValue);
}

static void RestoreStockLscPatches(
    const char* reason)
{
    bool restoreFailed = false;

    if (g_phase3SellCooldownHook.installed
        && g_phase3SellCooldownHook.program
        && g_phase3SellCooldownHook.original)
    {
        if (!WritePhase3NativeHandlerSlot(
                g_phase3SellCooldownHook.program,
                g_phase3SellCooldownHook.nativeIndex,
                g_phase3SellCooldownHook.original))
        {
            restoreFailed = true;
            Logf(
                "[Scope] failed restoring Sell cooldown native hook reason=%s",
                reason ? reason : "unspecified");
        }
        else
        {
            g_phase3SellCooldownHook =
                Phase3SellCooldownHook{};
        }
    }

    if (g_phase3SellDisplayPriceHook.installed
        && g_phase3SellDisplayPriceHook.program
        && g_phase3SellDisplayPriceHook.original)
    {
        if (!WritePhase3NativeHandlerSlot(
                g_phase3SellDisplayPriceHook.program,
                g_phase3SellDisplayPriceHook.nativeIndex,
                g_phase3SellDisplayPriceHook.original))
        {
            restoreFailed = true;
            Logf(
                "[Scope] failed restoring Sell display-price native hook reason=%s",
                reason ? reason : "unspecified");
        }
        else
        {
            g_phase3SellDisplayPriceHook =
                Phase3SellDisplayPriceHook{};
        }
    }

    if (g_sellOwnershipPatchApplied
        && g_sellOwnershipPatchedProgram)
    {
        bool ownershipRestored = true;

        for (size_t i = 0;
             i < g_sellOwnershipPatchBackups.size();
             ++i)
        {
            if (!WriteVmPatch(
                    g_sellOwnershipPatchedProgram,
                    g_sellOwnershipPatchBackups[i].position,
                    g_sellOwnershipPatchBackups[i].original))
            {
                ownershipRestored = false;
                break;
            }
        }

        if (!ownershipRestored)
        {
            restoreFailed = true;
            Logf(
                "[Scope] failed restoring Player_Vehicle Sell call patches reason=%s",
                reason ? reason : "unspecified");
        }
        else
        {
            g_sellOwnershipPatchApplied = false;
            g_sellOwnershipPatchedProgram = nullptr;
            g_sellOwnershipPatchPositions.clear();
            g_sellOwnershipPatchBackups.clear();
        }
    }

    if (g_highValueSellPatchApplied
        && g_highValueSellPatchedProgram)
    {
        if (!WriteVmPatch(
                g_highValueSellPatchedProgram,
                g_highValueSellPatchPosition,
                g_highValueSellOriginal))
        {
            restoreFailed = true;
            Logf(
                "[Scope] failed restoring high-value Sell patch reason=%s",
                reason ? reason : "unspecified");
        }
        else
        {
            g_highValueSellPatchApplied = false;
            g_highValueSellPatchedProgram = nullptr;
            g_highValueSellPatchPosition = 0;
            std::memset(
                g_highValueSellOriginal,
                0,
                sizeof(g_highValueSellOriginal));
        }
    }

    if (g_phase2PatchApplied
        && g_phase2PatchedProgram)
    {
        if (!WriteVmPatch(
                g_phase2PatchedProgram,
                g_phase2PatchPosition,
                g_phase2OriginalCall))
        {
            restoreFailed = true;
            Logf(
                "[Scope] failed restoring Sell visibility patch reason=%s",
                reason ? reason : "unspecified");
        }
        else
        {
            g_phase2PatchApplied = false;
            g_phase2VisibilityBypassActive = false;
            g_phase2PatchedProgram = nullptr;
            g_phase2PatchPosition = 0;
        }
    }

    if (restoreFailed)
        return;

    // Force fresh structural validation next time the player approaches an
    // actual Los Santos Customs. No patched state is carried to other garages.
    g_phase2AttemptedProgram = nullptr;
    g_phase3AnalyzedProgram = nullptr;
    g_phase3FunctionCatalog.clear();
    g_phase3SellHandler = VmFunctionRange{};
    g_phase3SellHandlerResolved = false;
    g_phase3SellEligibilityFunction =
        VmFunctionRange{};
    g_phase3NoSell1MessagePush = 0;
    g_phase3PlayerOwnedHelper =
        VmFunctionRange{};
    g_phase3SellPricePath =
        Phase3SellPricePath{};
    g_phase3SellCooldownPath =
        Phase3SellCooldownPath{};
    g_phase3SellControlPath =
        Phase3SellControlPath{};
    g_phase3SellStagePath =
        Phase3SellStagePath{};
    g_phase3PriceThreadInfo =
        Phase2ThreadInfo{};
    g_phase3PriceThreadCached = false;
    ResetPhase3PreparedSellPrice();
}

static bool IsPhase3PcAtNativeSite(
    uint32_t programCounter,
    uint32_t site)
{
    // The VM context can point at the NATIVE itself or just after its 4-byte
    // instruction while the handler is executing.
    return site != 0
        && programCounter >= site
        && programCounter <= site + 8;
}

static bool DecodePhase3HelperPredicateShape(
    Phase2ScrProgram* program,
    const Phase3HelperGateSite& helper,
    uint16_t& staticBaseIndex,
    int16_t& staticStateOffset,
    int& expectedPrimaryState,
    int& expectedSecondaryState)
{
    staticBaseIndex = 0;
    staticStateOffset = 0;
    expectedPrimaryState = 0;
    expectedSecondaryState = 0;

    if (!program || helper.nativeSite == 0)
        return false;

    const uint32_t inotPosition = helper.nativeSite + 4;
    unsigned char* inot = ScriptCodePointer(program, inotPosition);
    unsigned char* jz = ScriptCodePointer(program, inotPosition + 1);

    if (!inot || !jz || *inot != 0x06 || *jz != 0x56)
        return false;

    int16_t relative = 0;
    if (!ReadScriptSigned16(program, inotPosition + 2, relative))
        return false;

    const uint32_t onlineBody =
        static_cast<uint32_t>(
            static_cast<int64_t>(inotPosition + 1)
            + 3
            + static_cast<int64_t>(relative));

    unsigned char* staticLoad =
        ScriptCodePointer(program, onlineBody);
    if (!staticLoad || *staticLoad != 0x50)
        return false;

    uint32_t base = 0;
    if (!ReadScriptUnsigned(program, onlineBody + 1, 2, base)
        || base > 0xFFFFU)
    {
        return false;
    }

    uint32_t position = onlineBody + 3;
    int primary = 0;
    if (!TryGetVmPushedInt(program, position, primary))
        return false;

    uint32_t length = 0;
    if (!GetVmInstructionLength(program, position, length))
        return false;

    position += length;
    unsigned char* firstCompare =
        ScriptCodePointer(program, position);
    if (!firstCompare || *firstCompare != kVmIeqJz)
        return false;

    if (!GetVmInstructionLength(program, position, length))
        return false;
    position += length;

    unsigned char* staticAddress =
        ScriptCodePointer(program, position);
    if (!staticAddress || *staticAddress != 0x4F)
        return false;

    uint32_t secondBase = 0;
    if (!ReadScriptUnsigned(program, position + 1, 2, secondBase)
        || secondBase != base)
    {
        return false;
    }

    position += 3;

    // GTA V ScriptVM opcode 0x47 is IOFFSET_S16_LOAD.
    unsigned char* offsetLoad =
        ScriptCodePointer(program, position);
    if (!offsetLoad || *offsetLoad != 0x47)
        return false;

    uint32_t offsetRaw = 0;
    if (!ReadScriptUnsigned(program, position + 1, 2, offsetRaw))
        return false;

    const int16_t offset =
        static_cast<int16_t>(offsetRaw & 0xFFFFU);

    position += 3;

    int secondary = 0;
    if (!TryGetVmPushedInt(program, position, secondary))
        return false;

    staticBaseIndex = static_cast<uint16_t>(base);
    staticStateOffset = offset;
    expectedPrimaryState = primary;
    expectedSecondaryState = secondary;
    return true;
}

static void Phase3HelperNetworkProbe(Phase3NativeCallContext* context)
{
    Phase3NativeHandler original = g_phase3HelperNetworkOriginal;
    if (!original || original == &Phase3HelperNetworkProbe)
        return;

    original(context);

    if (!g_carmodShopActive
        || !g_rootMarkerSet
        || g_inferredMenuDepth <= 0
        || !g_phase3HelperNetworkInstalled
        || !g_phase3HelperTraceThread
        || g_phase3HelperTraceUntil == 0
        || GetTickCount64() > g_phase3HelperTraceUntil)
    {
        return;
    }

    uint32_t programCounter = 0;
    const size_t pcOffset = IsEnhancedEdition() ? 0x1C : 0x14;
    if (!ReadMemoryValue(
            g_phase3HelperTraceThread,
            pcOffset,
            programCounter))
    {
        return;
    }

    for (size_t i = 0; i < g_phase3HelperGateSiteCount; ++i)
    {
        const Phase3HelperGateSite& helper =
            g_phase3HelperGateSites[i];

        if (!IsPhase3PcAtNativeSite(
                programCounter,
                helper.nativeSite))
        {
            continue;
        }

        uint64_t returnRaw = 0;
        bool returnReadable = false;

        if (context
            && context->returnValue
            && IsReadableMemory(
                context->returnValue,
                sizeof(returnRaw)))
        {
            std::memcpy(
                &returnRaw,
                context->returnValue,
                sizeof(returnRaw));
            returnReadable = true;
        }

        int64_t primaryValue = 0;
        int64_t secondaryValue = 0;
        bool primaryReadable = false;
        bool secondaryReadable = false;
        int secondaryIndex = -1;

        if (helper.predicateShapeDecoded
            && g_phase3NativeProbeProgram
            && g_phase3NativeProbeProgram->localOffset)
        {
            const int baseIndex =
                static_cast<int>(helper.staticBaseIndex);
            secondaryIndex =
                baseIndex
                + static_cast<int>(helper.staticStateOffset);

            if (baseIndex >= 0
                && baseIndex < g_phase3NativeProbeProgram->localCount
                && IsReadableMemory(
                    g_phase3NativeProbeProgram->localOffset + baseIndex,
                    sizeof(int64_t)))
            {
                primaryValue =
                    g_phase3NativeProbeProgram->localOffset[baseIndex];
                primaryReadable = true;
            }

            if (secondaryIndex >= 0
                && secondaryIndex
                    < g_phase3NativeProbeProgram->localCount
                && IsReadableMemory(
                    g_phase3NativeProbeProgram->localOffset + secondaryIndex,
                    sizeof(int64_t)))
            {
                secondaryValue =
                    g_phase3NativeProbeProgram->localOffset[secondaryIndex];
                secondaryReadable = true;
            }
        }

        const bool wouldMatchIgnoringNetwork =
            helper.predicateShapeDecoded
            && primaryReadable
            && secondaryReadable
            && static_cast<int>(primaryValue)
                == helper.expectedPrimaryState
            && static_cast<int>(secondaryValue)
                == helper.expectedSecondaryState;

        Logf(
            "[Phase3Helper] HIT sequence=%u ordinal=%u callSite=0x%X targetFunc=%d targetStart=0x%X networkSite=0x%X networkReturn=%s raw=0x%llX predicateDecoded=%s staticBase=%u primary=%s%lld expectedPrimary=%d stateIndex=%d secondary=%s%lld expectedSecondary=%d wouldMatchIgnoringNetwork=%s tick=%llu",
            static_cast<unsigned int>(g_phase3HelperTraceSequence),
            static_cast<unsigned int>(i),
            helper.callSite,
            helper.functionIndex,
            helper.functionStart,
            helper.nativeSite,
            returnReadable
                ? ((returnRaw & 0xFFULL) != 0 ? "true" : "false")
                : "n/a",
            static_cast<unsigned long long>(returnRaw),
            helper.predicateShapeDecoded ? "yes" : "no",
            static_cast<unsigned int>(helper.staticBaseIndex),
            primaryReadable ? "" : "n/a:",
            static_cast<long long>(primaryValue),
            helper.expectedPrimaryState,
            secondaryIndex,
            secondaryReadable ? "" : "n/a:",
            static_cast<long long>(secondaryValue),
            helper.expectedSecondaryState,
            wouldMatchIgnoringNetwork ? "yes" : "no",
            static_cast<unsigned long long>(GetTickCount64()));
        return;
    }
}

static void RunPhase3NativeProbe(
    size_t slot,
    Phase3NativeCallContext* context)
{
    if (slot >= kPhase3NativeProbeCount)
        return;

    Phase3NativeProbe& probe = g_phase3NativeProbes[slot];
    Phase3NativeHandler original = probe.original;

    if (!original || original == probe.detour)
        return;

    // Phase 3F fast path: outside an active marked Sell submenu, these hooks
    // are pure pass-through. Do not inspect arguments, scan the thread array,
    // decode pointers, or touch the log file.
    if (!g_carmodShopActive
        || !g_rootMarkerSet
        || !g_phase3SellHandlerResolved
        || g_inferredMenuDepth <= 0)
    {
        original(context);
        ++probe.calls;
        return;
    }

    uint32_t contextArgCount = 0;
    uint64_t args[4]{};

    if (context)
    {
        contextArgCount = context->argCount;

        if (context->args
            && contextArgCount <= 64
            && IsReadableMemory(
                context->args,
                sizeof(uint64_t)
                    * static_cast<size_t>(
                        contextArgCount < 4 ? contextArgCount : 4)))
        {
            const uint64_t* rawArgs =
                reinterpret_cast<const uint64_t*>(context->args);

            const uint32_t copyCount =
                contextArgCount < 4 ? contextArgCount : 4;

            for (uint32_t i = 0; i < copyCount; ++i)
                args[i] = rawArgs[i];
        }
    }

    // Preserve Rockstar's original return value before inspecting the result.
    original(context);
    ++probe.calls;

    Phase2ThreadInfo threadInfo{};
    if (!GetPhase2ThreadInfo(kCarmodShopHash, threadInfo))
        return;

    const bool inSellHandler =
        threadInfo.programCounter >= g_phase3SellHandler.start
        && threadInfo.programCounter < g_phase3SellHandler.end;

    if (!inSellHandler)
        return;

    uint32_t matchedSite = 0;
    const char* matchedRole = probe.role;

    if (IsPhase3PcAtNativeSite(threadInfo.programCounter, probe.site))
    {
        matchedSite = probe.site;
    }
    else if (slot == 2
        && IsPhase3PcAtNativeSite(
            threadInfo.programCounter,
            g_phase3LaterStateGateASite))
    {
        matchedSite = g_phase3LaterStateGateASite;
        matchedRole = "later state gate A";
    }
    else if (slot == 3
        && IsPhase3PcAtNativeSite(
            threadInfo.programCounter,
            g_phase3LaterStateGateBSite))
    {
        matchedSite = g_phase3LaterStateGateBSite;
        matchedRole = "later state gate B";
    }
    else if (slot == 6
        && IsPhase3PcAtNativeSite(
            threadInfo.programCounter,
            g_phase3SecondaryStateTestSite))
    {
        matchedSite = g_phase3SecondaryStateTestSite;
        matchedRole = "state/bit test path B";
    }
    else if (slot == 8)
    {
        for (size_t i = 0; i < sizeof(g_phase3BitUpdateSites) / sizeof(g_phase3BitUpdateSites[0]); ++i)
        {
            if (IsPhase3PcAtNativeSite(
                    threadInfo.programCounter,
                    g_phase3BitUpdateSites[i]))
            {
                matchedSite = g_phase3BitUpdateSites[i];
                matchedRole = "bit/state update alternate";
                break;
            }
        }
    }
    else if (slot == 9)
    {
        for (size_t i = 0; i < sizeof(g_phase3DelaySites) / sizeof(g_phase3DelaySites[0]); ++i)
        {
            if (IsPhase3PcAtNativeSite(
                    threadInfo.programCounter,
                    g_phase3DelaySites[i]))
            {
                matchedSite = g_phase3DelaySites[i];
                matchedRole = "delay/yield action alternate";
                break;
            }
        }
    }

    if (matchedSite == 0)
    {
        if (probe.unmatchedInSellLogged < 3)
        {
            ++probe.unmatchedInSellLogged;
            Logf(
                "[Phase3Probe] unmatched-in-sell nativeIndex=%u pc=0x%X primarySite=0x%X laterA=0x%X laterB=0x%X stateTestB=0x%X",
                static_cast<unsigned int>(probe.nativeIndex),
                threadInfo.programCounter,
                probe.site,
                g_phase3LaterStateGateASite,
                g_phase3LaterStateGateBSite,
                g_phase3SecondaryStateTestSite);
        }

        return;
    }

    uint64_t returnRaw = 0;
    bool returnReadable = false;

    if (context
        && probe.expectedReturns > 0
        && context->returnValue
        && IsReadableMemory(context->returnValue, sizeof(uint64_t)))
    {
        std::memcpy(
            &returnRaw,
            context->returnValue,
            sizeof(returnRaw));
        returnReadable = true;
    }

    uint64_t frameLocal2 = 0;
    bool frameLocal2Readable = false;

    if (threadInfo.stack
        && threadInfo.framePointer + 2 < threadInfo.stackSize)
    {
        const unsigned char* local2 =
            reinterpret_cast<const unsigned char*>(threadInfo.stack)
            + static_cast<size_t>(
                threadInfo.framePointer + 2) * sizeof(uintptr_t);

        if (IsReadableMemory(local2, sizeof(frameLocal2)))
        {
            std::memcpy(&frameLocal2, local2, sizeof(frameLocal2));
            frameLocal2Readable = true;
        }
    }

    ++probe.matchedCalls;

    // Do not synchronously hit the log file for an identical state every frame.
    // We still count every matched call, but only state transitions are emitted.
    const bool duplicateState =
        probe.hasLoggedState
        && probe.lastLoggedSite == matchedSite
        && probe.lastLoggedContextArgCount == contextArgCount
        && probe.lastLoggedArg0 == args[0]
        && probe.lastLoggedArg1 == args[1]
        && probe.lastLoggedReturnReadable == returnReadable
        && (!returnReadable || probe.lastLoggedReturnRaw == returnRaw)
        && probe.lastLoggedFrameLocal2Readable == frameLocal2Readable
        && (!frameLocal2Readable
            || probe.lastLoggedFrameLocal2 == frameLocal2);

    if (duplicateState)
    {
        ++probe.suppressedDuplicateLogs;
        return;
    }

    probe.hasLoggedState = true;
    probe.lastLoggedSite = matchedSite;
    probe.lastLoggedContextArgCount = contextArgCount;
    probe.lastLoggedArg0 = args[0];
    probe.lastLoggedArg1 = args[1];
    probe.lastLoggedReturnRaw = returnRaw;
    probe.lastLoggedFrameLocal2 = frameLocal2;
    probe.lastLoggedReturnReadable = returnReadable;
    probe.lastLoggedFrameLocal2Readable = frameLocal2Readable;

    // Phase 3G: when the CMOD_SEL string-state gate transitions true, the
    // Sell handler immediately enters a short chain of helper functions that
    // each contain their own NETWORK_IS_GAME_IN_PROGRESS check. Arm a very
    // small synchronous trace window here so the helper network-native hook
    // can identify exactly which helper branches Rockstar executes next.
    if (slot == 7
        && returnReadable
        && (returnRaw & 0xFFULL) != 0
        && threadInfo.thread
        && g_phase3HelperNetworkInstalled)
    {
        g_phase3HelperTraceThread = threadInfo.thread;
        g_phase3HelperTraceUntil = GetTickCount64() + 250ULL;
        ++g_phase3HelperTraceSequence;

        Logf(
            "[Phase3Helper] ARMED sequence=%u reason=string-state-true helperCandidates=%u windowMs=250",
            static_cast<unsigned int>(g_phase3HelperTraceSequence),
            static_cast<unsigned int>(g_phase3HelperGateSiteCount));
    }

    const uint32_t arg0Low =
        static_cast<uint32_t>(args[0] & 0xFFFFFFFFULL);

    const bool arg0MatchesVehicle =
        probe.expectedArgs > 0
        && g_lastVehicleSnapshot.valid
        && arg0Low
            == static_cast<uint32_t>(g_lastVehicleSnapshot.vehicle);

    uint64_t arg0Pointee = 0;
    bool arg0PointeeReadable = false;

    if (args[0] > 0x10000ULL)
    {
        const void* arg0Pointer =
            reinterpret_cast<const void*>(
                static_cast<uintptr_t>(args[0]));

        if (IsReadableMemory(arg0Pointer, sizeof(uint64_t)))
        {
            std::memcpy(
                &arg0Pointee,
                arg0Pointer,
                sizeof(arg0Pointee));
            arg0PointeeReadable = true;
        }
    }

    char arg0Text[17]{};
    bool arg0TextReadable = false;

    if (args[0] > 0x10000ULL)
    {
        const char* textPointer =
            reinterpret_cast<const char*>(
                static_cast<uintptr_t>(args[0]));

        for (size_t i = 0; i < sizeof(arg0Text) - 1; ++i)
        {
            if (!IsReadableMemory(textPointer + i, 1))
                break;

            const unsigned char c =
                static_cast<unsigned char>(textPointer[i]);

            if (c == 0)
            {
                arg0TextReadable = true;
                break;
            }

            if (c < 0x20 || c > 0x7E)
                break;

            arg0Text[i] = static_cast<char>(c);

            if (i + 1 == sizeof(arg0Text) - 1)
                arg0TextReadable = true;
        }
    }

    Logf(
        "[Phase3Probe] MATCH role=%s site=0x%X nativeIndex=%u call=%u matched=%u suppressed=%u pc=0x%X fp=%u sp=%u expectedArgs=%u contextArgs=%u a0=0x%llX a1=0x%llX vehicleArg=%s a0Pointee=%s0x%llX a0Text=\"%s\" frameLocal2=%s0x%llX returnReadable=%s returnRaw=0x%llX returnBool=%s tick=%llu",
        matchedRole,
        matchedSite,
        static_cast<unsigned int>(probe.nativeIndex),
        static_cast<unsigned int>(probe.calls),
        static_cast<unsigned int>(probe.matchedCalls),
        static_cast<unsigned int>(probe.suppressedDuplicateLogs),
        threadInfo.programCounter,
        threadInfo.framePointer,
        threadInfo.stackPointer,
        static_cast<unsigned int>(probe.expectedArgs),
        static_cast<unsigned int>(contextArgCount),
        static_cast<unsigned long long>(args[0]),
        static_cast<unsigned long long>(args[1]),
        arg0MatchesVehicle ? "yes" : "no",
        arg0PointeeReadable ? "" : "n/a:",
        static_cast<unsigned long long>(arg0Pointee),
        arg0TextReadable ? arg0Text : "<n/a>",
        frameLocal2Readable ? "" : "n/a:",
        static_cast<unsigned long long>(frameLocal2),
        returnReadable ? "yes" : "no",
        static_cast<unsigned long long>(returnRaw),
        returnReadable
            ? ((returnRaw & 0xFFULL) != 0 ? "true" : "false")
            : "n/a",
        static_cast<unsigned long long>(GetTickCount64()));
}

struct Phase3NativeSite
{
    uint32_t position;
    uint16_t nativeIndex;
    uint8_t packed;
};

static bool InstallPhase3SellGateProbes(
    Phase2ScrProgram* program,
    const VmFunctionRange& sellHandler)
{
    if (!program
        || g_phase2NetworkGameNativeIndex == 0xFFFF
        || sellHandler.end <= sellHandler.start)
    {
        return false;
    }

    std::vector<Phase3NativeSite> natives;

    for (uint32_t position = sellHandler.start;
         position < sellHandler.end;)
    {
        unsigned char* opPtr = ScriptCodePointer(program, position);
        uint32_t length = 0;

        if (!opPtr || !GetVmInstructionLength(program, position, length))
            return false;

        if (*opPtr == kVmNative)
        {
            uint8_t packed = 0;
            uint16_t nativeIndex = 0;

            if (!ReadVmNativeSignature(
                    program,
                    position,
                    packed,
                    nativeIndex))
            {
                return false;
            }

            Phase3NativeSite site{};
            site.position = position;
            site.nativeIndex = nativeIndex;
            site.packed = packed;
            natives.push_back(site);
        }

        position += length;
    }

    int networkPosition = -1;
    int networkMatches = 0;

    for (size_t i = 0; i < natives.size(); ++i)
    {
        if (natives[i].nativeIndex
            == g_phase2NetworkGameNativeIndex)
        {
            ++networkMatches;
            networkPosition = static_cast<int>(i);
        }
    }

    if (networkMatches != 1
        || networkPosition < 2
        || static_cast<size_t>(networkPosition + 20)
            >= natives.size())
    {
        Logf(
            "[Phase3Probe] skipped reason=extended network native neighborhood not unique/complete matches=%d nativeCount=%u",
            networkMatches,
            static_cast<unsigned int>(natives.size()));
        return false;
    }

    const Phase3NativeSite selected[kPhase3NativeProbeCount] =
    {
        natives[static_cast<size_t>(networkPosition - 2)],
        natives[static_cast<size_t>(networkPosition - 1)],
        natives[static_cast<size_t>(networkPosition + 1)],
        natives[static_cast<size_t>(networkPosition + 2)],
        natives[static_cast<size_t>(networkPosition + 5)],
        natives[static_cast<size_t>(networkPosition + 7)],
        natives[static_cast<size_t>(networkPosition + 11)],
        natives[static_cast<size_t>(networkPosition + 14)],
        natives[static_cast<size_t>(networkPosition + 8)],
        natives[static_cast<size_t>(networkPosition + 10)],
    };

    const Phase3NativeSite laterStateGateA =
        natives[static_cast<size_t>(networkPosition + 4)];
    const Phase3NativeSite laterStateGateB =
        natives[static_cast<size_t>(networkPosition + 6)];
    const Phase3NativeSite secondaryStateTest =
        natives[static_cast<size_t>(networkPosition + 18)];
    const Phase3NativeSite bitUpdateAlternates[5] =
    {
        natives[static_cast<size_t>(networkPosition + 9)],
        natives[static_cast<size_t>(networkPosition + 12)],
        natives[static_cast<size_t>(networkPosition + 15)],
        natives[static_cast<size_t>(networkPosition + 16)],
        natives[static_cast<size_t>(networkPosition + 19)],
    };
    const Phase3NativeSite delayAlternates[3] =
    {
        natives[static_cast<size_t>(networkPosition + 13)],
        natives[static_cast<size_t>(networkPosition + 17)],
        natives[static_cast<size_t>(networkPosition + 20)],
    };

    const uint32_t networkSite =
        natives[static_cast<size_t>(networkPosition)].position;

    if (networkSite - selected[0].position > 64
        || delayAlternates[2].position - networkSite > 0x300)
    {
        Logf(
            "[Phase3Probe] skipped reason=extended native neighborhood too wide networkSite=0x%X first=0x%X last=0x%X",
            networkSite,
            selected[0].position,
            delayAlternates[2].position);
        return false;
    }

    if ((selected[0].packed >> 2) != 1
        || (selected[0].packed & 0x03) != 1
        || (selected[1].packed >> 2) != 1
        || (selected[1].packed & 0x03) != 1
        || (selected[2].packed >> 2) != 0
        || (selected[2].packed & 0x03) != 1
        || (selected[3].packed >> 2) != 0
        || (selected[3].packed & 0x03) != 1
        || (selected[4].packed >> 2) != 0
        || (selected[4].packed & 0x03) != 1
        || (selected[5].packed >> 2) != 0
        || (selected[5].packed & 0x03) != 1
        || (selected[6].packed >> 2) != 2
        || (selected[6].packed & 0x03) != 1
        || (selected[7].packed >> 2) != 1
        || (selected[7].packed & 0x03) != 1
        || (selected[8].packed >> 2) != 2
        || (selected[8].packed & 0x03) != 0
        || (selected[9].packed >> 2) != 1
        || (selected[9].packed & 0x03) != 0
        || laterStateGateA.nativeIndex != selected[2].nativeIndex
        || laterStateGateB.nativeIndex != selected[3].nativeIndex
        || (laterStateGateA.packed >> 2) != 0
        || (laterStateGateA.packed & 0x03) != 1
        || (laterStateGateB.packed >> 2) != 0
        || (laterStateGateB.packed & 0x03) != 1
        || secondaryStateTest.nativeIndex != selected[6].nativeIndex
        || (secondaryStateTest.packed >> 2) != 2
        || (secondaryStateTest.packed & 0x03) != 1
        || bitUpdateAlternates[0].nativeIndex != selected[8].nativeIndex
        || bitUpdateAlternates[1].nativeIndex != selected[8].nativeIndex
        || bitUpdateAlternates[2].nativeIndex != selected[8].nativeIndex
        || bitUpdateAlternates[3].nativeIndex != selected[8].nativeIndex
        || bitUpdateAlternates[4].nativeIndex != selected[8].nativeIndex
        || delayAlternates[0].nativeIndex != selected[9].nativeIndex
        || delayAlternates[1].nativeIndex != selected[9].nativeIndex
        || delayAlternates[2].nativeIndex != selected[9].nativeIndex
        || (bitUpdateAlternates[0].packed >> 2) != 2
        || (bitUpdateAlternates[0].packed & 0x03) != 0
        || (bitUpdateAlternates[1].packed >> 2) != 2
        || (bitUpdateAlternates[1].packed & 0x03) != 0
        || (bitUpdateAlternates[2].packed >> 2) != 2
        || (bitUpdateAlternates[2].packed & 0x03) != 0
        || (bitUpdateAlternates[3].packed >> 2) != 2
        || (bitUpdateAlternates[3].packed & 0x03) != 0
        || (bitUpdateAlternates[4].packed >> 2) != 2
        || (bitUpdateAlternates[4].packed & 0x03) != 0
        || (delayAlternates[0].packed >> 2) != 1
        || (delayAlternates[0].packed & 0x03) != 0
        || (delayAlternates[1].packed >> 2) != 1
        || (delayAlternates[1].packed & 0x03) != 0
        || (delayAlternates[2].packed >> 2) != 1
        || (delayAlternates[2].packed & 0x03) != 0)
    {
        Logf(
            "[Phase3Probe] skipped reason=extended native neighborhood signature mismatch");
        return false;
    }

    for (size_t i = 0; i < kPhase3NativeProbeCount; ++i)
    {
        for (size_t j = i + 1; j < kPhase3NativeProbeCount; ++j)
        {
            if (selected[i].nativeIndex == selected[j].nativeIndex)
            {
                Logf(
                    "[Phase3Probe] skipped reason=duplicate selected native index=%u",
                    static_cast<unsigned int>(selected[i].nativeIndex));
                return false;
            }
        }
    }

    if (program->nativeCount <= 0
        || !program->nativeOffset
        || !IsReadableMemory(
            program->nativeOffset,
            sizeof(Phase3NativeHandler)
                * static_cast<size_t>(program->nativeCount)))
    {
        Logf(
            "[Phase3Probe] skipped reason=invalid native table");
        return false;
    }

    ResetPhase3NativeProbeState();
    g_phase3LaterStateGateASite = laterStateGateA.position;
    g_phase3LaterStateGateBSite = laterStateGateB.position;
    g_phase3SecondaryStateTestSite = secondaryStateTest.position;

    for (size_t i = 0; i < sizeof(g_phase3BitUpdateSites) / sizeof(g_phase3BitUpdateSites[0]); ++i)
        g_phase3BitUpdateSites[i] = bitUpdateAlternates[i].position;

    for (size_t i = 0; i < sizeof(g_phase3DelaySites) / sizeof(g_phase3DelaySites[0]); ++i)
        g_phase3DelaySites[i] = delayAlternates[i].position;

    Phase3NativeHandler* table =
        reinterpret_cast<Phase3NativeHandler*>(program->nativeOffset);

    // Phase 3G: structurally collect the small helper functions called after
    // the CMOD_SEL string-state gate. We only retain direct callees that
    // contain exactly one NETWORK_IS_GAME_IN_PROGRESS native reference.
    // This avoids build-specific function numbers and gives the runtime helper
    // tracer a tiny set of exact native PCs to recognize.
    g_phase3HelperGateSiteCount = 0;

    uint32_t helperScan = selected[7].position + 4;
    while (helperScan < sellHandler.end)
    {
        unsigned char* opPtr =
            ScriptCodePointer(program, helperScan);
        uint32_t length = 0;

        if (!opPtr
            || !GetVmInstructionLength(
                program,
                helperScan,
                length))
        {
            break;
        }

        if (*opPtr == kVmCall)
        {
            uint32_t target = 0;
            if (ReadScriptUnsigned(
                    program,
                    helperScan + 1,
                    3,
                    target))
            {
                const VmFunctionRange* targetFunction =
                    FindVmFunctionByStart(
                        g_phase3FunctionCatalog,
                        target);

                if (targetFunction)
                {
                    std::vector<uint32_t> networkSites;
                    const int networkRefs =
                        CountNativeIndexReferences(
                            program,
                            *targetFunction,
                            g_phase2NetworkGameNativeIndex,
                            &networkSites);

                    if (networkRefs == 1
                        && networkSites.size() == 1
                        && g_phase3HelperGateSiteCount
                            < sizeof(g_phase3HelperGateSites)
                                / sizeof(g_phase3HelperGateSites[0]))
                    {
                        bool duplicateNativeSite = false;
                        for (size_t existing = 0;
                             existing < g_phase3HelperGateSiteCount;
                             ++existing)
                        {
                            if (g_phase3HelperGateSites[existing].nativeSite
                                == networkSites[0])
                            {
                                duplicateNativeSite = true;
                                break;
                            }
                        }

                        if (!duplicateNativeSite)
                        {
                            Phase3HelperGateSite& helper =
                                g_phase3HelperGateSites[
                                    g_phase3HelperGateSiteCount++];

                            helper.callSite = helperScan;
                            helper.functionStart =
                                targetFunction->start;
                            helper.nativeSite =
                                networkSites[0];
                            helper.functionIndex =
                                targetFunction->index;

                            helper.predicateShapeDecoded =
                                DecodePhase3HelperPredicateShape(
                                    program,
                                    helper,
                                    helper.staticBaseIndex,
                                    helper.staticStateOffset,
                                    helper.expectedPrimaryState,
                                    helper.expectedSecondaryState);
                        }
                    }
                }
            }
        }

        helperScan += length;
    }

    Logf(
        "[Phase3Helper] candidates=%u afterStringGate=0x%X",
        static_cast<unsigned int>(g_phase3HelperGateSiteCount),
        selected[7].position);

    for (size_t i = 0; i < g_phase3HelperGateSiteCount; ++i)
    {
        const Phase3HelperGateSite& helper =
            g_phase3HelperGateSites[i];

        const int staticStateIndex =
            static_cast<int>(helper.staticBaseIndex)
            + static_cast<int>(helper.staticStateOffset);

        Logf(
            "[Phase3Helper] candidate ordinal=%u callSite=0x%X targetFunc=%d targetStart=0x%X networkSite=0x%X predicateDecoded=%s staticBase=%u stateOffset=%d stateIndex=%d expectedPrimary=%d expectedSecondary=%d",
            static_cast<unsigned int>(i),
            helper.callSite,
            helper.functionIndex,
            helper.functionStart,
            helper.nativeSite,
            helper.predicateShapeDecoded ? "yes" : "no",
            static_cast<unsigned int>(helper.staticBaseIndex),
            static_cast<int>(helper.staticStateOffset),
            staticStateIndex,
            helper.expectedPrimaryState,
            helper.expectedSecondaryState);

        const VmFunctionRange* targetFunction =
            FindVmFunctionByStart(
                g_phase3FunctionCatalog,
                helper.functionStart);

        if (!targetFunction)
            continue;

        for (uint32_t helperPc = targetFunction->start;
             helperPc < targetFunction->end;)
        {
            uint32_t helperLength = 0;
            unsigned char* helperOp =
                ScriptCodePointer(program, helperPc);

            if (!helperOp
                || !GetVmInstructionLength(
                    program,
                    helperPc,
                    helperLength))
            {
                break;
            }

            LogPhase3FocusedInstruction(
                program,
                helperPc,
                helperLength,
                g_phase3FunctionCatalog);
            helperPc += helperLength;
        }
    }

    for (size_t i = 0; i < kPhase3NativeProbeCount; ++i)
    {
        if (selected[i].nativeIndex
            >= static_cast<uint16_t>(program->nativeCount))
        {
            Logf(
                "[Phase3Probe] skipped reason=native index out of range index=%u nativeCount=%d",
                static_cast<unsigned int>(selected[i].nativeIndex),
                program->nativeCount);
            ResetPhase3NativeProbeState();
            return false;
        }

        Phase3NativeProbe& probe = g_phase3NativeProbes[i];
        probe.nativeIndex = selected[i].nativeIndex;
        probe.site = selected[i].position;
        probe.expectedArgs =
            static_cast<uint8_t>(selected[i].packed >> 2);
        probe.expectedReturns =
            static_cast<uint8_t>(selected[i].packed & 0x03);
        probe.original = table[probe.nativeIndex];

        if (!probe.original
            || probe.original == probe.detour
            || !WritePhase3NativeHandlerSlot(
                program,
                probe.nativeIndex,
                probe.detour))
        {
            Logf(
                "[Phase3Probe] install FAILED safely role=%s site=0x%X nativeIndex=%u",
                probe.role,
                probe.site,
                static_cast<unsigned int>(probe.nativeIndex));

            for (size_t restore = 0; restore < i; ++restore)
            {
                Phase3NativeProbe& prior =
                    g_phase3NativeProbes[restore];

                if (prior.installed && prior.original)
                {
                    WritePhase3NativeHandlerSlot(
                        program,
                        prior.nativeIndex,
                        prior.original);
                    prior.installed = false;
                }
            }

            ResetPhase3NativeProbeState();
            return false;
        }

        probe.installed = true;

        Logf(
            "[Phase3Probe] installed role=%s site=0x%X nativeIndex=%u args=%u returns=%u original=%p detour=%p",
            probe.role,
            probe.site,
            static_cast<unsigned int>(probe.nativeIndex),
            static_cast<unsigned int>(probe.expectedArgs),
            static_cast<unsigned int>(probe.expectedReturns),
            reinterpret_cast<void*>(probe.original),
            reinterpret_cast<void*>(probe.detour));
    }

    if (g_phase3HelperGateSiteCount > 0
        && g_phase2NetworkGameNativeIndex
            < static_cast<uint16_t>(program->nativeCount))
    {
        g_phase3HelperNetworkOriginal =
            table[g_phase2NetworkGameNativeIndex];

        if (g_phase3HelperNetworkOriginal
            && g_phase3HelperNetworkOriginal
                != &Phase3HelperNetworkProbe
            && WritePhase3NativeHandlerSlot(
                program,
                g_phase2NetworkGameNativeIndex,
                &Phase3HelperNetworkProbe))
        {
            g_phase3HelperNetworkInstalled = true;

            Logf(
                "[Phase3Helper] network probe installed nativeIndex=%u original=%p detour=%p",
                static_cast<unsigned int>(
                    g_phase2NetworkGameNativeIndex),
                reinterpret_cast<void*>(
                    g_phase3HelperNetworkOriginal),
                reinterpret_cast<void*>(
                    &Phase3HelperNetworkProbe));
        }
        else
        {
            g_phase3HelperNetworkOriginal = nullptr;
            g_phase3HelperNetworkInstalled = false;

            Logf(
                "[Phase3Helper] network probe unavailable; helper tracing disabled");
        }
    }

    g_phase3NativeProbeProgram = program;

    Logf(
        "[Phase3Probe] READY scope=carmod_shop pass-through only laterStateA=0x%X laterStateB=0x%X stateTestB=0x%X bitUpdates=0x%X,0x%X,0x%X,0x%X,0x%X delays=0x%X,0x%X,0x%X. Logging is VM-site filtered; original handlers and return values are preserved.",
        g_phase3LaterStateGateASite,
        g_phase3LaterStateGateBSite,
        g_phase3SecondaryStateTestSite,
        g_phase3BitUpdateSites[0],
        g_phase3BitUpdateSites[1],
        g_phase3BitUpdateSites[2],
        g_phase3BitUpdateSites[3],
        g_phase3BitUpdateSites[4],
        g_phase3DelaySites[0],
        g_phase3DelaySites[1],
        g_phase3DelaySites[2]);

    return true;
}

static void UpdatePhase3Diagnostics()
{
    if (!g_phase3Enabled
        || g_lastNetworkGame
        || !g_phase2PatchApplied
        || g_phase2NetworkGameNativeIndex == 0xFFFF)
    {
        return;
    }

    if (g_phase3AnalyzedProgram
        && g_phase3AnalyzedProgram == g_phase2PatchedProgram)
    {
        return;
    }

    Phase2ScrProgram* program = FindPhase2Program(kCarmodShopHash);
    if (!program || program != g_phase2PatchedProgram)
        return;

    if (g_phase3AnalyzedProgram == program)
        return;

    g_phase3AnalyzedProgram = program;
    g_phase3SellHandlerResolved = false;
    g_phase3SellHandler = VmFunctionRange{};

    if (!ValidateProgramCode(program) || !ValidateProgramStrings(program))
    {
        Logf("[Phase3] Diagnostics failed safely: invalid code/string layout");
        return;
    }

    std::vector<VmFunctionRange> functions;
    if (!BuildVmFunctionCatalog(program, functions))
    {
        Logf("[Phase3] Diagnostics failed safely: function catalog failed");
        return;
    }

    g_phase3FunctionCatalog = functions;

    Logf(
        "[Phase3] Diagnostics begin program=%p networkGameNativeIndex=%u stringSize=%d",
        program,
        static_cast<unsigned int>(g_phase2NetworkGameNativeIndex),
        program->stringSize);

    VmFunctionRange sellHandler{};
    if (!ResolvePhase3SellHandler(program, functions, sellHandler))
        return;

    g_phase3SellHandler = sellHandler;
    g_phase3SellHandlerResolved = true;

    VmFunctionRange sellEligibilityFunction{};
    uint32_t noSell1MessagePush = 0;
    const bool sellEligibilityResolved =
        ResolvePhase3SellEligibilityFunction(
            program,
            functions,
            sellEligibilityFunction,
            noSell1MessagePush);

    if (sellEligibilityResolved)
    {
        g_phase3SellEligibilityFunction =
            sellEligibilityFunction;
        g_phase3NoSell1MessagePush =
            noSell1MessagePush;
    }

    const bool highValueSellBypassApplied =
        sellEligibilityResolved
        && ApplyHighValueSellRestrictionPatch(
            program,
            sellEligibilityFunction,
            noSell1MessagePush);

    VmFunctionRange playerOwnedHelper{};
    const bool playerOwnedHelperResolved =
        ResolvePhase3PlayerOwnedHelper(
            program,
            functions,
            playerOwnedHelper);

    if (playerOwnedHelperResolved)
        g_phase3PlayerOwnedHelper = playerOwnedHelper;

    const bool playerOwnedBypassApplied =
        sellEligibilityResolved
        && playerOwnedHelperResolved
        && ApplySellPlayerOwnedCallPatches(
            program,
            sellEligibilityFunction,
            noSell1MessagePush,
            sellHandler,
            playerOwnedHelper);

    LogPhase3FunctionOutline(program, sellHandler, functions);
    LogPhase3FocusedSellBytecode(program, sellHandler, functions);
    ResolvePhase3SellPricePath(
        program,
        sellHandler,
        functions);
    ResolvePhase3SellStagePath(
        program,
        functions,
        sellHandler);
    ResolvePhase3SellControlPath(
        program,
        sellHandler);
    const bool sellCooldownPathResolved =
        ResolvePhase3SellCooldownPath(
            program,
            sellHandler);

    // Keep the old synchronous diagnostic probes disabled. The one exception
    // is a single low-frequency REGISTER_SCRIPT_VARIABLE hook used only to
    // correct iOptionCost[0] before Rockstar composes the Sell price text.
    // It performs no thread scan, no VM discovery, and no synchronous logging.
    ResetPhase3NativeProbeState();

    const bool sellPriceDisplayHookInstalled =
        InstallPhase3SellDisplayPriceHook(
            program);
    const bool sellCooldownHookInstalled =
        sellCooldownPathResolved
        && InstallPhase3SellCooldownHook(
            program);

    Logf(
        "[Phase3] Diagnostics READY nativeProbes=disabled runtimeTrace=disabled sellEligibility=%s highValueSellBypass=%s playerOwnedHelper=%s playerOwnedBypass=%s sellPricePath=%s sellPriceDisplayHook=%s sellCooldownPath=%s sellCooldownHook=%s. Phase 3M bypasses the CMOD_NOSELL1 high-value rejection and forces the Player_Vehicle ownership predicate true only at Sell-related CALL sites. The display-price and cooldown hooks are exact-site filtered and perform no runtime VM/thread scans.",
        sellEligibilityResolved ? "resolved" : "unresolved",
        highValueSellBypassApplied ? "yes" : "no",
        playerOwnedHelperResolved ? "resolved" : "unresolved",
        playerOwnedBypassApplied ? "yes" : "no",
        g_phase3SellPricePath.resolved ? "resolved" : "unresolved",
        sellPriceDisplayHookInstalled ? "yes" : "no",
        sellCooldownPathResolved ? "resolved" : "unresolved",
        sellCooldownHookInstalled ? "yes" : "no");
}

static void ArmPhase3RuntimeTrace(
    const char* reason,
    int depthBefore,
    int depthAfter)
{
    if (!g_phase3Enabled || !g_carmodShopActive)
        return;

    g_phase3TraceUntil = GetTickCount64() + 4000ULL;
    g_phase3LastTracePc = 0xFFFFFFFFU;
    g_phase3LastTraceFp = 0xFFFFFFFFU;
    g_phase3LastTraceSp = 0xFFFFFFFFU;
    g_phase3LastTraceState = 0xFFFFFFFFU;

    Logf(
        "[Phase3Trace] ARMED reason=%s session=%u inputSeq=%u depthBefore=%d depthAfter=%d windowMs=4000",
        reason ? reason : "unspecified",
        static_cast<unsigned int>(g_shopSessionId),
        static_cast<unsigned int>(g_inputSequence),
        depthBefore,
        depthAfter);
}

static void PollPhase3RuntimeTrace(ULONGLONG now)
{
    if (!g_phase3Enabled
        || !g_carmodShopActive
        || g_phase3TraceUntil == 0)
    {
        return;
    }

    if (now > g_phase3TraceUntil)
    {
        Logf(
            "[Phase3Trace] END session=%u lastPc=0x%X lastFp=%u lastSp=%u lastState=%u",
            static_cast<unsigned int>(g_shopSessionId),
            g_phase3LastTracePc == 0xFFFFFFFFU ? 0U : g_phase3LastTracePc,
            g_phase3LastTraceFp == 0xFFFFFFFFU ? 0U : g_phase3LastTraceFp,
            g_phase3LastTraceSp == 0xFFFFFFFFU ? 0U : g_phase3LastTraceSp,
            g_phase3LastTraceState == 0xFFFFFFFFU ? 0U : g_phase3LastTraceState);
        g_phase3TraceUntil = 0;
        return;
    }

    Phase2ThreadInfo info{};
    if (!GetPhase2ThreadInfo(kCarmodShopHash, info))
        return;

    if (info.programCounter == g_phase3LastTracePc
        && info.framePointer == g_phase3LastTraceFp
        && info.stackPointer == g_phase3LastTraceSp
        && info.threadState == g_phase3LastTraceState)
    {
        return;
    }

    g_phase3LastTracePc = info.programCounter;
    g_phase3LastTraceFp = info.framePointer;
    g_phase3LastTraceSp = info.stackPointer;
    g_phase3LastTraceState = info.threadState;

    int functionIndex = -1;
    bool inSellHandler = false;

    if (!g_phase3FunctionCatalog.empty())
    {
        const VmFunctionRange* function =
            FindVmFunctionContaining(
                g_phase3FunctionCatalog,
                info.programCounter);
        functionIndex = function ? function->index : -1;
    }

    if (g_phase3SellHandlerResolved)
    {
        inSellHandler =
            info.programCounter >= g_phase3SellHandler.start
            && info.programCounter < g_phase3SellHandler.end;
    }

    Logf(
        "[Phase3Trace] sample session=%u pc=0x%X func=%d state=%u fp=%u sp=%u stackSize=%u inSellHandler=%s",
        static_cast<unsigned int>(g_shopSessionId),
        info.programCounter,
        functionIndex,
        info.threadState,
        info.framePointer,
        info.stackPointer,
        info.stackSize,
        inSellHandler ? "yes" : "no");
}

static void LogPhase2RuntimeState(const char* reason)
{
    Phase2ScrProgram* program = FindPhase2Program(kCarmodShopHash);

    Phase2ThreadInfo threadInfo{};
    const bool threadFound = GetPhase2ThreadInfo(kCarmodShopHash, threadInfo);

    bool local764Readable = false;
    if (threadFound && threadInfo.stack && threadInfo.stackSize > 764)
    {
        const unsigned char* local764 =
            reinterpret_cast<const unsigned char*>(threadInfo.stack)
            + static_cast<size_t>(764) * sizeof(uintptr_t);

        local764Readable = IsReadableMemory(local764, sizeof(uintptr_t));
    }

    Logf(
        "[Phase2Runtime] reason=%s session=%u programFound=%s program=%p patchApplied=%s patchCode=0x%X threadFound=%s thread=%p stack=%p threadId=%u state=%u pc=0x%X fp=%u sp=%u stackSize=%u Local_764_readable=%s status=%s",
        reason ? reason : "unspecified",
        static_cast<unsigned int>(g_shopSessionId),
        program ? "yes" : "no",
        program,
        g_phase2PatchApplied && program == g_phase2PatchedProgram ? "yes" : "no",
        g_phase2PatchPosition,
        threadFound ? "yes" : "no",
        threadFound ? threadInfo.thread : nullptr,
        threadFound ? threadInfo.stack : nullptr,
        threadFound ? threadInfo.threadId : 0U,
        threadFound ? threadInfo.threadState : 0U,
        threadFound ? threadInfo.programCounter : 0U,
        threadFound ? threadInfo.framePointer : 0U,
        threadFound ? threadInfo.stackPointer : 0U,
        threadFound ? threadInfo.stackSize : 0U,
        local764Readable ? "yes" : "no",
        g_phase2Status.c_str());
}

static bool DoesScriptExistByHash(uint32_t scriptHash)
{
    nativeInit(kDoesScriptWithNameHashExistNative);
    nativePush64(static_cast<uint64_t>(scriptHash));
    uint64_t* result = nativeCall();
    return result && (*result != 0);
}

static bool HasScriptLoadedByHash(uint32_t scriptHash)
{
    nativeInit(kHasScriptWithNameHashLoadedNative);
    nativePush64(static_cast<uint64_t>(scriptHash));
    uint64_t* result = nativeCall();
    return result && (*result != 0);
}

static int GetRunningScriptCountByHash(uint32_t scriptHash)
{
    nativeInit(kGetNumberOfThreadsRunningScriptHashNative);
    nativePush64(static_cast<uint64_t>(scriptHash));
    uint64_t* result = nativeCall();

    if (!result)
        return 0;

    return static_cast<int>(*result);
}

static void InitializeScriptProbes()
{
    for (size_t i = 0; i < kScriptProbeCount; ++i)
    {
        ScriptProbe& probe = g_scriptProbes[i];
        probe.hash = Joaat(probe.name);
        probe.exists = DoesScriptExistByHash(probe.hash);
        probe.loaded = HasScriptLoadedByHash(probe.hash);
        probe.runningCount =
            GetRunningScriptCountByHash(probe.hash);
        probe.initialized = true;

        Logf(
            "[ScriptCatalog] name=%s hash=0x%08X exists=%s loaded=%s running=%d",
            probe.name,
            static_cast<unsigned int>(probe.hash),
            probe.exists ? "yes" : "no",
            probe.loaded ? "yes" : "no",
            probe.runningCount);
    }
}

static VehicleSnapshot CaptureVehicleSnapshot()
{
    VehicleSnapshot snapshot{};
    snapshot.vehicle = 0;
    snapshot.model = 0;
    snapshot.vehicleClass = -1;
    snapshot.modKit = -1;
    snapshot.entityHealth = 0;
    snapshot.engineHealth = 0.0f;
    snapshot.bodyHealth = 0.0f;
    snapshot.coords = Vector3();
    snapshot.valid = false;

    const Ped playerPed = PLAYER::PLAYER_PED_ID();
    if (!PED::IS_PED_IN_ANY_VEHICLE(playerPed, false))
        return snapshot;

    const Vehicle vehicle =
        PED::GET_VEHICLE_PED_IS_IN(playerPed, false);

    if (vehicle == 0 || !ENTITY::DOES_ENTITY_EXIST(vehicle))
        return snapshot;

    snapshot.vehicle = vehicle;
    snapshot.model = ENTITY::GET_ENTITY_MODEL(vehicle);
    snapshot.vehicleClass = VEHICLE::GET_VEHICLE_CLASS(vehicle);
    snapshot.modKit = VEHICLE::GET_VEHICLE_MOD_KIT(vehicle);
    snapshot.entityHealth = ENTITY::GET_ENTITY_HEALTH(vehicle);
    snapshot.engineHealth = VEHICLE::GET_VEHICLE_ENGINE_HEALTH(vehicle);
    snapshot.bodyHealth = VEHICLE::GET_VEHICLE_BODY_HEALTH(vehicle);
    snapshot.coords = ENTITY::GET_ENTITY_COORDS(vehicle, true);
    snapshot.valid = true;

    return snapshot;
}

static bool VehicleSnapshotChanged(
    const VehicleSnapshot& left,
    const VehicleSnapshot& right)
{
    if (left.valid != right.valid)
        return true;

    if (!left.valid)
        return false;

    return left.vehicle != right.vehicle
        || left.model != right.model
        || left.vehicleClass != right.vehicleClass
        || left.modKit != right.modKit;
}

static void LogVehicleSnapshot(
    const char* reason,
    bool force)
{
    if (!g_logVehicleSnapshots)
        return;

    const VehicleSnapshot snapshot =
        CaptureVehicleSnapshot();

    if (!force
        && !VehicleSnapshotChanged(
            snapshot,
            g_lastVehicleSnapshot))
    {
        return;
    }

    if (!snapshot.valid)
    {
        Logf(
            "[Vehicle] session=%u reason=%s vehicle=<none>",
            static_cast<unsigned int>(g_shopSessionId),
            reason ? reason : "unspecified");
    }
    else
    {
        Logf(
            "[Vehicle] session=%u reason=%s handle=%d model=0x%08X class=%d modKit=%d entityHealth=%d engineHealth=%.2f bodyHealth=%.2f pos=(%.3f, %.3f, %.3f)",
            static_cast<unsigned int>(g_shopSessionId),
            reason ? reason : "unspecified",
            static_cast<int>(snapshot.vehicle),
            static_cast<unsigned int>(snapshot.model),
            snapshot.vehicleClass,
            snapshot.modKit,
            snapshot.entityHealth,
            snapshot.engineHealth,
            snapshot.bodyHealth,
            snapshot.coords.x,
            snapshot.coords.y,
            snapshot.coords.z);
    }

    g_lastVehicleSnapshot = snapshot;
}

static bool WasFrontendControlJustPressed(int control)
{
    // Depending on the current carmod_shop menu state Rockstar can leave the
    // frontend control enabled or disable it and query the disabled-control
    // state. Check both paths so Phase 3 tracing does not miss confirmation
    // presses when the menu changes input ownership.
    return CONTROLS::IS_CONTROL_JUST_PRESSED(2, control)
        || CONTROLS::IS_DISABLED_CONTROL_JUST_PRESSED(2, control);
}

static bool LogControlIfPressed(
    int control,
    const char* name)
{
    if (!g_carmodShopActive)
        return false;

    if (!WasFrontendControlJustPressed(control))
        return false;

    if (!g_logControls)
        return true;

    const int depthBefore = g_inferredMenuDepth;

    const bool acceptControl =
        control == 201
        || control == 237;

    const bool cancelControl =
        control == 202
        || control == 238;

    if (g_rootMarkerSet)
    {
        if (acceptControl)
            ++g_inferredMenuDepth;
        else if (cancelControl && g_inferredMenuDepth > 0)
            --g_inferredMenuDepth;
    }

    ++g_inputSequence;

    Logf(
        "[MenuInput] seq=%u session=%u control=%s id=%d depthBefore=%d depthAfter=%d rootMarked=%s gameTimer=%d",
        static_cast<unsigned int>(g_inputSequence),
        static_cast<unsigned int>(g_shopSessionId),
        name,
        control,
        depthBefore,
        g_inferredMenuDepth,
        g_rootMarkerSet ? "yes" : "no",
        GAMEPLAY::GET_GAME_TIMER());

    LogVehicleSnapshot("menu input", false);
    LogPhase3SellPriceState("menu input");

    // Phase 3K: do not arm runtime Sell tracing from menu accepts.
    // Menu input/depth diagnostics remain available without per-frame VM polling.
    return true;
}

static void PollRelevantControls(
    bool& acceptPressed,
    bool& cancelPressed)
{
    acceptPressed = false;
    cancelPressed = false;

    if (!g_carmodShopActive)
        return;

    if (!g_logControls)
    {
        acceptPressed =
            WasFrontendControlJustPressed(201)
            || WasFrontendControlJustPressed(237);

        cancelPressed =
            WasFrontendControlJustPressed(202)
            || WasFrontendControlJustPressed(238);
        return;
    }

    LogControlIfPressed(187, "FRONTEND_DOWN");
    LogControlIfPressed(188, "FRONTEND_UP");
    LogControlIfPressed(189, "FRONTEND_LEFT");
    LogControlIfPressed(190, "FRONTEND_RIGHT");

    const bool frontendAccept =
        LogControlIfPressed(201, "FRONTEND_ACCEPT");
    const bool frontendCancel =
        LogControlIfPressed(202, "FRONTEND_CANCEL");
    const bool cursorAccept =
        LogControlIfPressed(237, "CURSOR_ACCEPT");
    const bool cursorCancel =
        LogControlIfPressed(238, "CURSOR_CANCEL");

    LogControlIfPressed(205, "FRONTEND_LB");
    LogControlIfPressed(206, "FRONTEND_RB");

    acceptPressed =
        frontendAccept
        || cursorAccept;

    cancelPressed =
        frontendCancel
        || cursorCancel;
}

static void LogManualMarker()
{
    const SHORT currentState = GetAsyncKeyState(VK_F10);
    const bool pressedNow = (currentState & 0x8000) != 0;
    const bool pressedBefore = (g_lastF10State & 0x8000) != 0;
    g_lastF10State = currentState;

    if (!pressedNow || pressedBefore)
        return;

    if (g_carmodShopActive)
    {
        g_rootMarkerSet = true;
        g_inferredMenuDepth = 0;

        for (size_t i = 0; i < kPhase3NativeProbeCount; ++i)
        {
            Phase3NativeProbe& probe = g_phase3NativeProbes[i];
            probe.hasLoggedState = false;
            probe.lastLoggedSite = 0;
            probe.lastLoggedContextArgCount = 0;
            probe.lastLoggedArg0 = 0;
            probe.lastLoggedArg1 = 0;
            probe.lastLoggedReturnRaw = 0;
            probe.lastLoggedFrameLocal2 = 0;
            probe.lastLoggedReturnReadable = false;
            probe.lastLoggedFrameLocal2Readable = false;
            probe.suppressedDuplicateLogs = 0;
        }

        Logf(
            "[MenuTrace] ROOT_MARKER session=%u inputSeq=%u networkGame=%s gameTimer=%d",
            static_cast<unsigned int>(g_shopSessionId),
            static_cast<unsigned int>(g_inputSequence),
            NETWORK::NETWORK_IS_GAME_IN_PROGRESS() ? "yes" : "no",
            GAMEPLAY::GET_GAME_TIMER());

        LogVehicleSnapshot("root menu marker", true);
        LogPhase2RuntimeState("root menu marker");

        Notify(
            "~b~SellVehiclesAtLSC~w~: Root menu marker recorded.");
    }
    else
    {
        Logf(
            "[Marker] F10 pressed outside carmod_shop. session=%u gameTimer=%d",
            static_cast<unsigned int>(g_shopSessionId),
            GAMEPLAY::GET_GAME_TIMER());
    }
}

static void UpdateNetworkState()
{
    const bool networkGame =
        NETWORK::NETWORK_IS_GAME_IN_PROGRESS();

    if (!g_networkStateInitialized
        || networkGame != g_lastNetworkGame)
    {
        Logf(
            "[Mode] NETWORK_IS_GAME_IN_PROGRESS=%s",
            networkGame ? "true" : "false");

        g_lastNetworkGame = networkGame;
        g_networkStateInitialized = true;
    }
}

static void BeginCarmodShopSession()
{
    ++g_shopSessionId;
    g_shopSessionStartedAt = GetTickCount64();
    g_nextSnapshotAt = 0;
    g_lastVehicleSnapshot = VehicleSnapshot{};
    g_rootMarkerSet = false;
    g_inferredMenuDepth = -1;
    g_inputSequence = 0;
    g_phase3TraceUntil = 0;
    g_phase3FallbackVehicle = 0;
    g_phase3FallbackModel = 0;
    g_phase3FallbackPrice = 0;
    g_phase3FallbackLogged = false;
    g_phase3SellContextActive = false;
    g_phase3SellContextPrice = 0;
    g_phase3SellControlState = -1;
    g_phase3LastLoggedSellControlState = -999;
    g_phase3CurrentMenuState = -1;
    g_phase3LastLoggedMenuState = -999;
    g_phase3SellStageActive = false;
    g_phase3PriceThreadInfo = Phase2ThreadInfo{};
    g_phase3PriceThreadCached = false;
    g_nextPhase3PriceUpdateAt = 0;
    g_nextPhase2ProgramCheckAt = 0;

    ApplyCharacterVehicleSettingForShop();

    Logf(
        "[ShopSession] BEGIN session=%u gameTimer=%d networkGame=%s",
        static_cast<unsigned int>(g_shopSessionId),
        GAMEPLAY::GET_GAME_TIMER(),
        NETWORK::NETWORK_IS_GAME_IN_PROGRESS() ? "yes" : "no");

    LogVehicleSnapshot("carmod_shop started", true);
    LogPhase2RuntimeState("carmod_shop started");
}

static void EndCarmodShopSession()
{
    const ULONGLONG now = GetTickCount64();
    const ULONGLONG elapsed =
        now >= g_shopSessionStartedAt
            ? now - g_shopSessionStartedAt
            : 0ULL;

    Logf(
        "[ShopSession] END session=%u elapsedMs=%llu gameTimer=%d",
        static_cast<unsigned int>(g_shopSessionId),
        static_cast<unsigned long long>(elapsed),
        GAMEPLAY::GET_GAME_TIMER());

    LogVehicleSnapshot("carmod_shop stopped", true);
    RestoreCharacterVehicleSettingAfterShop();

    g_phase3SellContextActive = false;
    g_phase3SellContextPrice = 0;
    g_phase3SellControlState = -1;
    g_phase3LastLoggedSellControlState = -999;
    g_phase3CurrentMenuState = -1;
    g_phase3LastLoggedMenuState = -999;
    g_phase3SellStageActive = false;
    g_phase3PriceThreadInfo = Phase2ThreadInfo{};
    g_phase3PriceThreadCached = false;
    g_nextPhase3PriceUpdateAt = 0;
    g_nextPhase2ProgramCheckAt = 0;

    if (g_phase3Enabled && g_rootMarkerSet)
    {
        for (size_t i = 0; i < kPhase3NativeProbeCount; ++i)
        {
            const Phase3NativeProbe& probe = g_phase3NativeProbes[i];

            if (probe.installed
                && (probe.matchedCalls > 0
                    || probe.suppressedDuplicateLogs > 0))
            {
                Logf(
                    "[Phase3Probe] SUMMARY role=%s nativeIndex=%u calls=%u matched=%u suppressedDuplicates=%u",
                    probe.role,
                    static_cast<unsigned int>(probe.nativeIndex),
                    static_cast<unsigned int>(probe.calls),
                    static_cast<unsigned int>(probe.matchedCalls),
                    static_cast<unsigned int>(probe.suppressedDuplicateLogs));
            }
        }
    }

    g_rootMarkerSet = false;
    g_inferredMenuDepth = -1;
}

static void PollScriptStates()
{
    bool carmodShopNowActive = false;

    // carmod_shop is the only probe needed to maintain active-session state.
    // Once it is running, do not keep querying the six unrelated diagnostic
    // scripts every poll; that work is useful for discovery logs but not for
    // the live Sell path.
    const size_t probeCount =
        g_carmodShopActive ? 1 : kScriptProbeCount;

    for (size_t i = 0; i < probeCount; ++i)
    {
        ScriptProbe& probe = g_scriptProbes[i];

        const bool exists = DoesScriptExistByHash(probe.hash);
        const bool loaded = HasScriptLoadedByHash(probe.hash);
        const int runningCount =
            GetRunningScriptCountByHash(probe.hash);

        if (!probe.initialized
            || exists != probe.exists
            || loaded != probe.loaded
            || runningCount != probe.runningCount)
        {
            Logf(
                "[ScriptState] name=%s hash=0x%08X exists=%s->%s loaded=%s->%s running=%d->%d",
                probe.name,
                static_cast<unsigned int>(probe.hash),
                probe.exists ? "yes" : "no",
                exists ? "yes" : "no",
                probe.loaded ? "yes" : "no",
                loaded ? "yes" : "no",
                probe.runningCount,
                runningCount);
        }

        probe.exists = exists;
        probe.loaded = loaded;
        probe.runningCount = runningCount;
        probe.initialized = true;

        if (i == 0)
            carmodShopNowActive = runningCount > 0;
    }

    if (carmodShopNowActive != g_carmodShopActive)
    {
        g_carmodShopActive = carmodShopNowActive;

        if (g_carmodShopActive)
            BeginCarmodShopSession();
        else
            EndCarmodShopSession();
    }
}

static void PollPeriodicSnapshot(ULONGLONG now)
{
    if (!g_carmodShopActive
        || !g_logVehicleSnapshots
        || now < g_nextSnapshotAt)
    {
        return;
    }

    g_nextSnapshotAt =
        now + static_cast<ULONGLONG>(g_snapshotIntervalMs);

    LogVehicleSnapshot("periodic shop snapshot", false);
}

static void LoadSettings()
{
    g_enabled =
        ReadIniBool(
            "Settings",
            "Enabled",
            true);

    g_useSellCooldown =
        ReadIniBool(
            "Settings",
            "UseSellCooldown",
            false);

    g_sellCooldownMinutes =
        ReadIniInt(
            "Settings",
            "SellCooldownMinutes",
            48);

    if (g_sellCooldownMinutes < 1)
        g_sellCooldownMinutes = 1;

    g_vehicleSellPercent =
        ReadIniInt(
            "Settings",
            "VehicleSellPercent",
            60);

    g_upgradePercent =
        ReadIniInt(
            "Settings",
            "UpgradePercent",
            8);

    g_useDamagePenalty =
        ReadIniBool(
            "Settings",
            "UseDamagePenalty",
            true);

    g_allowCharacterVehicles =
        ReadIniBool(
            "Settings",
            "AllowCharacterVehicles",
            false);

    g_showStartupNotification =
        ReadIniBool(
            "Settings",
            "Notification",
            false);

    // Keep percentages flexible for modders while preventing accidental
    // negative values or extreme overflow-prone settings.
    if (g_vehicleSellPercent < 0)
        g_vehicleSellPercent = 0;
    else if (g_vehicleSellPercent > 1000)
        g_vehicleSellPercent = 1000;

    if (g_upgradePercent < 0)
        g_upgradePercent = 0;
    else if (g_upgradePercent > 1000)
        g_upgradePercent = 1000;

    const bool legacyLogEnabled =
        ReadIniInt(
            "Diagnostics",
            "EnableLog",
            1) != 0;

    g_logEnabled =
        ReadIniBool(
            "Settings",
            "Logging",
            legacyLogEnabled);

    g_logControls =
        ReadIniInt("Diagnostics", "LogControls", 0) != 0;

    g_logVehicleSnapshots =
        ReadIniInt(
            "Diagnostics",
            "LogVehicleSnapshots",
            0) != 0;

    g_scriptPollIntervalMs =
        ReadIniInt(
            "Diagnostics",
            "ScriptPollIntervalMs",
            250);

    g_snapshotIntervalMs =
        ReadIniInt(
            "Diagnostics",
            "SnapshotIntervalMs",
            1000);

    g_phase2Enabled =
        ReadIniInt(
            "Phase2",
            "EnableSellVisibilityPatch",
            1) != 0;

    g_phase3Enabled =
        ReadIniInt(
            "Phase3",
            "EnableSellFlowDiagnostics",
            1) != 0;

    if (g_scriptPollIntervalMs < 50)
        g_scriptPollIntervalMs = 50;

    if (g_scriptPollIntervalMs > 5000)
        g_scriptPollIntervalMs = 5000;

    if (g_snapshotIntervalMs < 250)
        g_snapshotIntervalMs = 250;

    if (g_snapshotIntervalMs > 10000)
        g_snapshotIntervalMs = 10000;
}

static void LogStartupState()
{
    Logf("SellVehiclesAtLSC starting...");
    Logf("[Info] BuildTag=%s", kBuildTag);
    Logf("[Info] Executable=%s", GetExecutableName());
    Logf("[Info] Edition=%s", GetEditionName());
    Logf("[Info] getGameVersion()=%d", getGameVersion());
    Logf(
        "[Info] Settings enabled=%s logging=%s notification=%s useSellCooldown=%s sellCooldownMinutes=%d vehicleSellPercent=%d upgradePercent=%d useDamagePenalty=%s allowCharacterVehicles=%s controls=%s vehicleSnapshots=%s scriptPollMs=%d snapshotMs=%d phase2=%s phase3=%s",
        g_enabled ? "yes" : "no",
        g_logEnabled ? "on" : "off",
        g_showStartupNotification ? "on" : "off",
        g_useSellCooldown ? "yes" : "no",
        g_sellCooldownMinutes,
        g_vehicleSellPercent,
        g_upgradePercent,
        g_useDamagePenalty ? "yes" : "no",
        g_allowCharacterVehicles ? "yes" : "no",
        g_logControls ? "on" : "off",
        g_logVehicleSnapshots ? "on" : "off",
        g_scriptPollIntervalMs,
        g_snapshotIntervalMs,
        g_phase2Enabled ? "on" : "off",
        g_phase3Enabled ? "on" : "off");
    Logf("[Info] Phase 2 preserves the Phase 1B diagnostics and structurally resolves carmod_shop's category-42 visibility call at runtime. It does not use decompiler function numbers, spoof NETWORK_IS_GAME_IN_PROGRESS, or write script locals, vehicle state, or money state.");
    Logf("[Info] Phase 3N uses GTACars-derived purchase prices as the primary stock-value reference for known native GTA vehicles. VehicleSellPercent=%d applies to corrected stock value and UpgradePercent=%d applies to estimated installed-upgrade retail value before the final price is injected into Rockstar's registered iOptionCost[0]. Missing/newer/add-on models use the class/model fallback.", g_vehicleSellPercent, g_upgradePercent);
    Logf("[Info] Phase 3O adds a separate SellCompletion controller. While the resolved native Sell price field is active, it follows Rockstar's two-step Sell confirmation, then fades out, removes the sold vehicle, moves the player to the nearest stock LSC exterior, and fades back in.");
    Logf("[Info] v0.3.5 performance: carmod_shop program discovery is rate-limited, completed Phase 3 analysis takes a zero-scan fast path, Sell-price runtime state caches the resolved script thread and samples the price slot at 20 Hz instead of scanning the full script-thread array every frame, and network/script diagnostics use the timed poll instead of the per-frame Sell path.");
    Logf("[Info] SellCompletion keeps the validated Sell-stage + iControl trigger unchanged, preserves the 2000 ms post-confirm delay, credits the final dynamically resolved sale price to the active Story Mode character's persistent SP*_TOTAL_CASH account after the transition completes, and briefly shows the native Story Mode cash balance after payout.");
    Logf("[Info] Gameplay cooldown preserves Rockstar's native CMOD_NOSELL3 Sell gate. Story Mode rejects writes to MPPLY_VEHICLE_SELL_TIME, so an exact-site clock hook supplies the elapsed value only to Rockstar's original cooldown comparison. SellCooldownMinutes controls that elapsed window without patching the rejection message or unrelated eligibility checks.");
    Logf("[Info] UseDamagePenalty=%s scales the configured resale price by the worse of body/engine condition. AllowCharacterVehicles=%s uses Rockstar's SP protagonist model+plate definitions to suppress the injected Sell category for those character vehicles.", g_useDamagePenalty ? "on" : "off", g_allowCharacterVehicles ? "yes" : "no");
    Logf("[Info] Scope: VM patches and native hooks are enabled only within 140m of the four stock Los Santos Customs locations (Burton, LSIA, La Mesa, Harmony). They are restored outside that scope; Beeker's and other customization garages are not patched.");
    Logf("[Info] Performance rule: no heavy per-frame scans or repeated structural discovery are permitted in the live LSC path; expensive work must remain cached, event-driven, or rate-limited.");
    Logf("[Info] Test workflow: enter Story Mode LSC, open Sell, confirm the sale normally, then verify the 2000 ms pause, fade-out, vehicle removal, exterior teleport, fade-in, and one-time Story Mode payout matching the captured Sell price.");
}

void ScriptMain()
{
    LoadSettings();

    if (g_logEnabled)
        ResetLogFile();

    if (!g_enabled)
    {
        Logf("[Info] Enabled=false; SellVehiclesAtLSC is disabled.");
        FlushLogBuffer();
        return;
    }

    SellCompletion::Initialize(
        &LogSellCompletionMessage);
    SellCompletion::ConfigureCooldown(
        g_useSellCooldown,
        g_sellCooldownMinutes);

    LogStartupState();
    InitializeScriptProbes();
    UpdateNetworkState();
    const bool phase2Ready =
        InitializePhase2Internals();

    if (phase2Ready)
    {
        WriteStatusLogLine(
            "SellVehiclesAtLSC Initialized");
    }
    else
    {
        Logf(
            "[Init] SellVehiclesAtLSC initialization failed: Phase 2 internals unavailable");
    }

    if (g_showStartupNotification)
    {
        Notify(
            "~b~~h~SellVehiclesAtLSC~h~~w~ enabled.");
    }

    // PERFORMANCE RULE:
    // Keep the live LSC tick lightweight. Do not add full VM/thread scans,
    // repeated structural discovery, synchronous file I/O, or other expensive
    // work here. Cache stable results and make heavier work event-driven or
    // rate-limited outside the per-frame path.
    while (true)
    {
        WAIT(0);

        const ULONGLONG now = GetTickCount64();

        if (now >= g_nextLogFlushAt)
        {
            if (!g_carmodShopActive)
                FlushLogBuffer();

            g_nextLogFlushAt =
                now + kLogFlushIntervalMs;
        }

        if (now >= g_nextScriptPollAt)
        {
            g_nextScriptPollAt =
                now + static_cast<ULONGLONG>(
                    g_scriptPollIntervalMs);

            UpdateNetworkState();
            PollScriptStates();
        }

        UpdatePhase2SellExposure();
        UpdatePhase3Diagnostics();
        UpdatePhase3SellPriceFallback();
        UpdatePhase3SellControlStateFast();
        LogManualMarker();

        if (g_carmodShopActive
            && g_logControls)
        {
            bool acceptPressed = false;
            bool cancelPressed = false;

            PollRelevantControls(
                acceptPressed,
                cancelPressed);
        }

        UpdateSellCompletionController();
        FlushPhase3SellCooldownGateEvent();

        PollPeriodicSnapshot(now);
    }
}
