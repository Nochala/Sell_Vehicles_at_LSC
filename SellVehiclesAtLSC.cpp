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
static const char* kBuildTag = "v0.3.4 Phase 3O post-sale transition";

static bool g_logEnabled = true;
static bool g_showStartupNotification = true;
static bool g_logControls = true;
static bool g_logVehicleSnapshots = true;
static int g_scriptPollIntervalMs = 250;
static int g_snapshotIntervalMs = 1000;

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
    { "personal_carmod_shop", 0, false, false, 0, false },
    { "car_meet_carmod", 0, false, false, 0, false },
    { "business_hub_carmod", 0, false, false, 0, false },
    { "arena_carmod", 0, false, false, 0, false },
    { "armory_aircraft_carmod", 0, false, false, 0, false },
    { "am_car_mod_tut", 0, false, false, 0, false },
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
static SHORT g_lastF10State = 0;
static bool g_rootMarkerSet = false;
static int g_inferredMenuDepth = -1;
static uint32_t g_inputSequence = 0;

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

static void ResetLogFile()
{
    if (!g_logEnabled)
        return;

    FILE* file = _fsopen(g_logPath, "w", _SH_DENYNO);
    if (file)
        fclose(file);
}

static void Logf(const char* format, ...)
{
    if (!g_logEnabled || !format)
        return;

    FILE* file = _fsopen(g_logPath, "a", _SH_DENYNO);
    if (!file)
        return;

    SYSTEMTIME localTime{};
    GetLocalTime(&localTime);

    std::fprintf(
        file,
        "[%02u:%02u:%02u.%03u] ",
        static_cast<unsigned int>(localTime.wHour),
        static_cast<unsigned int>(localTime.wMinute),
        static_cast<unsigned int>(localTime.wSecond),
        static_cast<unsigned int>(localTime.wMilliseconds));

    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);

    std::fputc('\n', file);
    std::fflush(file);
    std::fclose(file);
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

static Phase2ScrProgram* g_highValueSellPatchedProgram = nullptr;
static uint32_t g_highValueSellPatchPosition = 0;
static bool g_highValueSellPatchApplied = false;

static VmFunctionRange g_phase3SellEligibilityFunction{};
static uint32_t g_phase3NoSell1MessagePush = 0;

static Phase2ScrProgram* g_sellOwnershipPatchedProgram = nullptr;
static std::vector<uint32_t> g_sellOwnershipPatchPositions;
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
};

static Phase3SellPricePath g_phase3SellPricePath{};
static Vehicle g_phase3FallbackVehicle = 0;
static Hash g_phase3FallbackModel = 0;
static int g_phase3FallbackPrice = 0;
static bool g_phase3FallbackLogged = false;
static bool g_phase3SellContextActive = false;
static int g_phase3SellContextPrice = 0;

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

    // The structurally resolved Sell filter takes one vehicle argument and
    // returns one bool. Replacing only this call with DROP + false keeps the
    // VM stack balanced while bypassing the Story Mode hide decision for
    // category 42 alone.
    const unsigned char patch[4] =
    {
        kVmDrop, kVmPushConst0, kVmNop, kVmNop
    };

    if (!WriteVmPatch(program, callPosition, patch))
    {
        g_phase2Status = "VM patch write failed";
        Logf("[Phase2] SellExposed=no reason=VM patch write/verify failed");
        return false;
    }

    g_phase2PatchedProgram = program;
    g_phase2PatchPosition = callPosition;
    g_phase2PatchApplied = true;
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

static void UpdatePhase2SellExposure()
{
    if (!g_phase2Enabled
        || NETWORK::NETWORK_IS_GAME_IN_PROGRESS()
        || !InitializePhase2Internals())
    {
        return;
    }

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
            g_phase3SellEligibilityFunction = VmFunctionRange{};
            g_phase3NoSell1MessagePush = 0;
            g_phase3PlayerOwnedHelper = VmFunctionRange{};

            if (program != g_highValueSellPatchedProgram)
            {
                g_highValueSellPatchedProgram = nullptr;
                g_highValueSellPatchPosition = 0;
                g_highValueSellPatchApplied = false;
            }

            if (program != g_sellOwnershipPatchedProgram)
            {
                g_sellOwnershipPatchedProgram = nullptr;
                g_sellOwnershipPatchPositions.clear();
                g_sellOwnershipPatchApplied = false;
            }
        }

        if (g_phase2PatchedProgram && program != g_phase2PatchedProgram)
        {
            g_phase2PatchedProgram = nullptr;
            g_phase2PatchPosition = 0;
            g_phase2PatchApplied = false;
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

    Logf(
        "[Phase3L] SellPricePath=yes itemCostPush=0x%X priceLoad=0x%X priceAddress=0x%X initializerCall=0x%X initializerFunc=%d@0x%X staticBase=%u fieldOffset=%d stride=%u element0StaticIndex=%u validatedUses=%d",
        itemCostPush,
        priceLoadPosition,
        priceAddressPosition,
        initializerCall,
        initializerFunction.index,
        initializerFunction.start,
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

static void UpdatePhase3SellPriceFallback()
{
    g_phase3SellContextActive = false;
    g_phase3SellContextPrice = 0;

    if (!g_phase3Enabled
        || !g_phase3SellPricePath.resolved
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
        return;
    }

    const uint32_t index =
        g_phase3SellPricePath.element0StaticIndex;

    if (index >= threadInfo.stackSize)
        return;

    unsigned char* slot =
        reinterpret_cast<unsigned char*>(
            threadInfo.stack)
        + static_cast<size_t>(index)
            * sizeof(uintptr_t);

    uint64_t raw = 0;
    if (!IsReadableMemory(slot, sizeof(raw)))
        return;

    std::memcpy(&raw, slot, sizeof(raw));

    const int32_t rockstarPrice =
        static_cast<int32_t>(
            raw & 0xFFFFFFFFULL);

    if (rockstarPrice > 0)
    {
        g_phase3SellContextActive = true;
        g_phase3SellContextPrice =
            static_cast<int>(rockstarPrice);
        return;
    }

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

    int fallbackPrice = g_phase3FallbackPrice;

    if (fallbackPrice <= 0)
    {
        const int modelValue =
            GetPhase3VehicleModelValue(
                model);

        if (modelValue <= 0)
        {
            if (!g_phase3FallbackLogged)
            {
                Logf(
                    "[Phase3N] SellPriceFallback=no vehicle=%d model=0x%08X rockstarPrice=%d modelValue=%d reason=model value unavailable",
                    static_cast<int>(vehicle),
                    static_cast<unsigned int>(model),
                    static_cast<int>(rockstarPrice),
                    modelValue);
                g_phase3FallbackLogged = true;
            }

            return;
        }

        const int64_t scaled =
            static_cast<int64_t>(modelValue) * 60LL;

        fallbackPrice =
            static_cast<int>(scaled / 100LL);

        if (fallbackPrice <= 0)
            fallbackPrice = modelValue;

        g_phase3FallbackPrice = fallbackPrice;
    }

    const uint64_t patchedRaw =
        (raw & 0xFFFFFFFF00000000ULL)
        | static_cast<uint32_t>(
            fallbackPrice);

    std::memcpy(
        slot,
        &patchedRaw,
        sizeof(patchedRaw));

    uint64_t verify = 0;
    if (!IsReadableMemory(slot, sizeof(verify)))
        return;

    std::memcpy(&verify, slot, sizeof(verify));

    const int32_t verifiedPrice =
        static_cast<int32_t>(
            verify & 0xFFFFFFFFULL);

    if (verifiedPrice != fallbackPrice)
    {
        if (!g_phase3FallbackLogged)
        {
            Logf(
                "[Phase3N] SellPriceFallback=no vehicle=%d model=0x%08X rockstarPrice=%d fallback=%d reason=write verification failed observed=%d",
                static_cast<int>(vehicle),
                static_cast<unsigned int>(model),
                static_cast<int>(rockstarPrice),
                fallbackPrice,
                static_cast<int>(verifiedPrice));
            g_phase3FallbackLogged = true;
        }

        return;
    }

    g_phase3SellContextActive = true;
    g_phase3SellContextPrice = fallbackPrice;

    if (!g_phase3FallbackLogged)
    {
        Logf(
            "[Phase3N] SellPriceFallback=yes vehicle=%d model=0x%08X rockstarPrice=%d fallback=%d basis=GET_VEHICLE_MODEL_VALUE_60pct staticIndex=%u",
            static_cast<int>(vehicle),
            static_cast<unsigned int>(model),
            static_cast<int>(rockstarPrice),
            fallbackPrice,
            static_cast<unsigned int>(index));
        g_phase3FallbackLogged = true;
    }
}

static void UpdateSellCompletionController(
    bool acceptPressed,
    bool cancelPressed)
{
    SellCompletion::Update(
        g_carmodShopActive,
        g_phase3SellContextActive,
        g_phase3SellContextPrice,
        acceptPressed,
        cancelPressed);
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
        || NETWORK::NETWORK_IS_GAME_IN_PROGRESS()
        || !g_phase2PatchApplied
        || g_phase2NetworkGameNativeIndex == 0xFFFF)
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

    // Phase 3K: do not install synchronous native detours in carmod_shop.
    // Even pass-through probes execute on Rockstar's hot Sell path and can
    // introduce severe hitching during confirmation/transaction processing.
    // Keep the one-time structural diagnostics above, but leave runtime native
    // handlers completely untouched.
    ResetPhase3NativeProbeState();

    Logf(
        "[Phase3] Diagnostics READY nativeProbes=disabled runtimeTrace=disabled sellEligibility=%s highValueSellBypass=%s playerOwnedHelper=%s playerOwnedBypass=%s sellPricePath=%s. Phase 3M bypasses the CMOD_NOSELL1 high-value rejection and forces the Player_Vehicle ownership predicate true only at Sell-related CALL sites. No global ownership helper, network state, payout, deletion, or money state is modified.",
        sellEligibilityResolved ? "resolved" : "unresolved",
        highValueSellBypassApplied ? "yes" : "no",
        playerOwnedHelperResolved ? "resolved" : "unresolved",
        playerOwnedBypassApplied ? "yes" : "no",
        g_phase3SellPricePath.resolved ? "resolved" : "unresolved");
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

    for (size_t i = 0; i < kScriptProbeCount; ++i)
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
    g_logEnabled =
        ReadIniInt("Diagnostics", "EnableLog", 1) != 0;

    g_showStartupNotification =
        ReadIniInt(
            "Diagnostics",
            "ShowStartupNotification",
            1) != 0;

    g_logControls =
        ReadIniInt("Diagnostics", "LogControls", 1) != 0;

    g_logVehicleSnapshots =
        ReadIniInt(
            "Diagnostics",
            "LogVehicleSnapshots",
            1) != 0;

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
        "[Info] Settings log=%s startupNotification=%s controls=%s vehicleSnapshots=%s scriptPollMs=%d snapshotMs=%d phase2=%s phase3=%s",
        g_logEnabled ? "on" : "off",
        g_showStartupNotification ? "on" : "off",
        g_logControls ? "on" : "off",
        g_logVehicleSnapshots ? "on" : "off",
        g_scriptPollIntervalMs,
        g_snapshotIntervalMs,
        g_phase2Enabled ? "on" : "off",
        g_phase3Enabled ? "on" : "off");
    Logf("[Info] Phase 2 preserves the Phase 1B diagnostics and structurally resolves carmod_shop's category-42 visibility call at runtime. It does not use decompiler function numbers, spoof NETWORK_IS_GAME_IN_PROGRESS, or write script locals, vehicle state, or money state.");
    Logf("[Info] Phase 3N keeps the Phase 3M eligibility/ownership patches unchanged and adds a narrow Sell-price fallback. When Rockstar's structurally resolved ITEM_COST field is zero/invalid in Story Mode, the mod writes 60%% of GET_VEHICLE_MODEL_VALUE into that same field. Positive Rockstar prices are never overridden.");
    Logf("[Info] Phase 3O adds a separate SellCompletion controller. While the resolved native Sell price field is active, it follows Rockstar's two-step Sell confirmation, then fades out, removes the sold vehicle, moves the player to the nearest stock LSC exterior, and fades back in.");
    Logf("[Info] SellCompletion does not replace Rockstar's Sell menu, payout, eligibility, or price logic. It adds only post-confirm cleanup/transition behavior and does not install synchronous native detours.");
    Logf("[Info] Test workflow: enter Story Mode LSC, open Sell, confirm the sale normally, then verify fade-out, vehicle removal, exterior teleport, and fade-in. Send the log if any step does not complete.");
}

void ScriptMain()
{
    LoadSettings();

    if (g_logEnabled)
        ResetLogFile();

    SellCompletion::Initialize(
        &LogSellCompletionMessage);

    LogStartupState();
    InitializeScriptProbes();
    UpdateNetworkState();
    InitializePhase2Internals();

    if (g_showStartupNotification)
    {
        Notify(
            "~b~~h~SellVehiclesAtLSC~h~~w~ Phase 3O post-sale transition enabled.");
    }

    while (true)
    {
        WAIT(0);

        const ULONGLONG now = GetTickCount64();

        UpdateNetworkState();
        UpdatePhase2SellExposure();
        UpdatePhase3Diagnostics();
        UpdatePhase3SellPriceFallback();
        LogManualMarker();

        if (now >= g_nextScriptPollAt)
        {
            g_nextScriptPollAt =
                now + static_cast<ULONGLONG>(
                    g_scriptPollIntervalMs);

            PollScriptStates();
        }

        bool acceptPressed = false;
        bool cancelPressed = false;

        if (g_carmodShopActive)
        {
            PollRelevantControls(
                acceptPressed,
                cancelPressed);
        }

        UpdateSellCompletionController(
            acceptPressed,
            cancelPressed);

        PollPeriodicSnapshot(now);
    }
}
