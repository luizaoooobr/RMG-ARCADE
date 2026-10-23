/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#define _CRT_SECURE_NO_WARNINGS
#define CORE_INTERNAL
#include "FbneoRuntime.hpp"
#include "Error.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// FBNeo public headers use a TCHAR type. RMG-Core is non-UNICODE, so
// force TCHAR == char regardless of any platform headers pulled in.
#ifndef _TCHAR_DEFINED
typedef char TCHAR;
#define _TCHAR_DEFINED
#endif
#ifndef _T
#define _T(x) x
#endif

extern "C" {
#include "burn.h"
}

// BurnStateCompress/BurnStateDecompress live in statec.cpp and are declared in
// no public header. They are NOT wrapped in extern "C" there, so they get C++
// linkage (verified in libfbneo_core.a: _Z17BurnStateCompressPPhPii and
// _Z19BurnStateDecompressPhii). Declaring them as extern "C" here would ask the
// linker for unmangled names and fail, so they must keep C++ linkage.
extern INT32 BurnStateCompress(UINT8** pDef, INT32* pnDefLen, INT32 bAll);
extern INT32 BurnStateDecompress(UINT8* Def, INT32 nDefLen, INT32 bAll);

// Sound-only state snapshot for rollback (defined in the FBNeo overlay,
// capcom/cps_mem.cpp; C++ linkage like the rest of the lib symbols). Capture
// saves ONLY the sound-chip state (Z80 + YM2151/OKI/QSound + timer + latches);
// replay restores it. Used so rollback resims do not re-trigger already
// audible voices (SFX overlap "toca-discos" on netplay).
extern INT32 RMGK_FBNEO_SndStateSnapshot();
extern INT32 RMGK_FBNEO_SndStateReplay();

static int      rmgkSndAudioFramesEmitted = 0;   // audible-history depth (gates the snapshot)
static bool     rmgkSndPending            = false;
static long long rmgkSndPendingUs         = 0;
static int      rmgkSndRestores           = 0;

static bool rmgkSndArm()
{
    // Only arm once ~0.75s of audible history has passed so startup savestate
    // loads never replay garbage/pre-init chip state.
    if (rmgkSndAudioFramesEmitted < 45) return false;
    if (RMGK_FBNEO_SndStateSnapshot() > 0)
    {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        rmgkSndPendingUs = (long long)std::chrono::duration_cast<std::chrono::microseconds>(now).count();
        return true;
    }
    return false;
}

static void rmgkSndMaybeReplay()
{
    if (!rmgkSndPending) return;
    rmgkSndPending = false;
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const long long nowUs = (long long)std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    // A stale snapshot (e.g. captured at session teardown then re-arm never
    // happened) must not replay on an unrelated session: snapshots only stay
    // valid for one rollback batch (a few ms), allow a generous 2s.
    if (nowUs - rmgkSndPendingUs > 2000000LL) return;    // drop stale
    RMGK_FBNEO_SndStateReplay();
    rmgkSndRestores++;
}

// Global variable at global scope: the Itanium ABI does not mangle these, so the
// symbol is literally "nRMGKNetgameMode" on both sides (no extern "C" needed).
// Defined in FBNeo's src/burner/rmgk/frontend.cpp; backing storage for
// is_netgame_or_recording(). Must be set BEFORE fbneo_rt::Initialize().
extern int nRMGKNetgameMode;

// Full-loop-iteration phase mark (defined in rmgk_gekko.cpp). Indices mirror
// RmgkLoopMark: 3 = RunFrame entry, 4 = RunFrame exit, 5 = present entry,
// 6 = present exit. No-op unless the pacing trace is enabled.
extern "C" void rmgk_gekko_trace_loop_mark(int which);

// The zip helpers live in src/burner/zipfn.cpp, which the lib frontend links in.
// Same story as above: C++ linkage, so no extern "C". ZipGetList()/ZipLoadFile()
// let BurnExtLoadRom resolve a ROM by CRC and copy it *clamped* to the driver's
// declared size (ZipLoadOneFile would read the whole entry and overflow the
// caller's buffer whenever the archive held a larger file). The declarations
// must match src/burner/burner.h verbatim or the C++ name mangling won't resolve.
struct ZipEntry { char* szName; UINT32 nLen; UINT32 nCrc; };
extern INT32 ZipOpen(char* szZip);
extern INT32 ZipClose();
extern INT32 ZipGetList(struct ZipEntry** pList, INT32* pnListCount);
extern INT32 ZipLoadFile(UINT8* Dest, INT32 nLen, INT32* pnWrote, INT32 nEntry);

namespace fbneo_rt
{
namespace
{
using clock_ = std::chrono::steady_clock;

bool         g_libInited     = false;
bool         g_driverInited  = false;
int          g_players       = 2;
std::vector<uint32_t> g_localMasks;
// The frontend writes local input from the GUI thread (key/button events) while
// the emulation thread reads it every frame in SampleInput, so the mask array
// is guarded by this mutex.
std::mutex   g_localInputMutex;

int (*g_inputCallback)(void*, int, int) = nullptr;
int  g_inputPlayers  = 0;
int  g_inputSize     = 4;
std::vector<uint8_t> g_latchedInput;

FrameCallback g_presentCallback = nullptr;
void*         g_presentUser     = nullptr;
int           g_orientationFlags = 0;

// Archives that make up the romset. g_archives holds the *open name* (extension
// stripped, because ZipOpen() re-appends it); g_archiveFiles keeps the original
// path for diagnostics.
std::vector<std::string> g_archives;
std::vector<std::string> g_archiveFiles;
std::string              g_driverName;

// One runtime input slot. For a plain digital input `targets` holds a single
// driver input index; for a FBNeo-style combination macro (e.g. "P1 Buttons AB")
// it holds every index the macro drives at once.
struct PlayerInputMap { std::vector<std::vector<UINT32> > targets; std::vector<std::string> names; };
std::vector<PlayerInputMap> g_playerInputMap;
// g_playerInputMap is built when a session starts (emulation thread) but the
// frontend reads the input names from the GUI thread while building key maps,
// so the table is guarded by this mutex.
std::mutex g_playerInputMapMutex;

// Driver short-name -> full display name. The rom browser queries this for every
// archive in a directory, and walking the driver table mutates the global
// nBurnDrvActive. So the table is built once into a cache; later lookups (which
// may run on the search thread while a game is running) are pure reads.
std::mutex                                 g_driverNameMutex;
std::unordered_map<std::string, std::string> g_driverNameCache;

// FBNeo's BurnLibInit() calls BurnLibExit() internally, and BurnLibExit() (via
// BurnGameListExit) frees the driver-name tables without nulling the pointers.
// Invoking BurnLibInit() again after a BurnLibExit() therefore double-frees and
// corrupts the heap (STATUS_HEAP_CORRUPTION) as soon as the second game starts.
// Keep the coarse library tables for the whole process and release them once at
// exit, so BurnLibInit() only ever runs against a clean state.
void fbneo_lib_atexit()
{
    if (g_libInited)
    {
        BurnLibExit();
        g_libInited = false;
    }
}

bool ensure_lib_init()
{
    if (g_libInited)
    {
        return true;
    }
    if (BurnLibInit() != 0)
    {
        return false;
    }
    g_libInited = true;
    std::atexit(fbneo_lib_atexit);
    return true;
}

std::vector<uint8_t>   g_videoBuffer;
VideoFrame             g_videoFrame;

std::vector<int16_t>   g_soundBuffer;
int                    g_soundFrames = 0;

m64p_rollback_run_frame_stats g_stats = {};

// Serializes access to the live driver state so GUI-thread commands (reset,
// save/load state) never run in the middle of a BurnDrvFrame() stepped on the
// emulation thread. Frames hold it for their whole body; state commands hold it
// while they read/write the driver, so they execute cleanly between frames.
std::mutex g_frameMutex;

// A reset requested from another thread (the GUI / System menu). It is applied
// on the emulation thread at the top of the next RunFrame, while the frame lock
// is held, so the driver is never torn down underneath a running frame or the
// presenter (which reads pBurnDraw without the lock).
std::atomic_bool g_pendingReset{false};

// Pause flag for arcade sessions. When set, RunFrame keeps the presenter fed
// with the last rendered frame (and silence) but skips stepping the driver, so
// the window never goes black and the rollback/pacing loop stays alive. Written
// from the GUI thread (System > Pause), read on the emulation thread.
std::atomic_bool g_paused{false};

// DIP switch byte overrides the frontend asked to keep (BurnInputInfo index ->
// value), applied on top of the driver defaults so they survive a driver reset.
// Guarded by g_dipMutex (never hold g_frameMutex while acquiring it in reverse;
// callers that touch driver bytes take g_frameMutex first, then g_dipMutex).
std::vector<std::pair<int, uint8_t> > g_dipOverrides;
std::mutex g_dipMutex;

int lower_c(int c) { return (c >= 'A' && c <= 'Z') ? (c - 'A' + 'a') : c; }

std::string basename_lower(const std::string& p)
{
    std::string out;
    for (char c : p) out.push_back((char)lower_c((unsigned char)c));
    std::string::size_type slash = out.find_last_of("/\\");
    return slash == std::string::npos ? out : out.substr(slash + 1);
}

uint32_t fnv1a(const uint8_t* data, size_t len)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= data[i]; h *= 16777619u; }
    return h;
}

// Deterministic 128-bit driver identifier, formatted as 32 hex chars (see
// DriverRomId). Built from four salted FNV-1a passes so both peers always
// derive the same value from the driver's normalized short name.
std::string driver_rom_id(const std::string& normalizedName)
{
    static const uint32_t seeds[4] = { 0x811c9dc5u, 0x01000193u, 0x9e3779b9u, 0x85ebca6bu };
    char out[33];
    for (int s = 0; s < 4; s++)
    {
        uint32_t h = seeds[s];
        for (unsigned char c : normalizedName) { h ^= c; h *= 16777619u; }
        std::snprintf(out + s * 8, 9, "%08x", (unsigned)h);
    }
    return std::string(out, 32);
}

// burn.h exposes BurnHighCol as a function pointer whose default initialiser
// (burn.cpp) is a filler that returns 0xffffffff -- pure white. The interactive
// frontends replace it through SetBurnHighCol(), which lives in the burner/misc
// layer that the headless 'lib' frontend does not build. Left alone, every
// palette entry collapses to white, so the whole screen is white. Point it at a
// real 32-bpp RGB converter (identical to misc.cpp's HighCol24) instead.
UINT32 __cdecl fbneo_highcol32(INT32 r, INT32 g, INT32 b, INT32 /* i */)
{
    return ((UINT32)r << 16) | ((UINT32)g << 8) | (UINT32)b;
}

// FBNeo's BurnLoadRomExt() asks the application to decode ROM index i into the
// caller's Dest buffer, sized to the driver's declared length. Real FBNeo
// frontends resolve a ROM by its CRC32 (the archive cache is keyed by CRC), and
// that is also how romsets that store a file under a different name still load
// -- e.g. the NeoGeo BIOS "sp-s3.sp1" is shipped as "asia-s3.rom" in the very
// same neogeo.zip. We therefore match by CRC32 first and fall back to the name.
//
// We must never copy more than the driver's declared nLen: the destination is
// exactly that size, so loading a larger archive entry straight into it would
// overflow and corrupt the heap. ZipLoadFile() clamps the read to nLen; the old
// ZipLoadOneFile() did not, which crashed the emulator on mismatched romsets.
//
// NOTE on the ".zip" suffix: ZipOpen() rebuilds the path as "<name>.zip" /
// "<name>.7z", so the archive must be handed over WITHOUT its extension.
// g_archives stores exactly that "open name"; g_archiveFiles keeps the original
// path for diagnostics.
INT32 __cdecl fbneo_load_rom(UINT8* dest, INT32* pnWrote, INT32 i)
{
    struct BurnRomInfo ri;
    memset(&ri, 0, sizeof(ri));
    BurnDrvGetRomInfo(&ri, i);
    if (ri.nType == 0 || ri.nLen == 0) { if (pnWrote) *pnWrote = 0; return 1; }

    char* name = nullptr;
    BurnDrvGetRomName(&name, i, 0);
    const std::string want = basename_lower(name ? name : ri.szName ? ri.szName : "");

    for (size_t a = 0; a < g_archives.size(); a++)
    {
        if (ZipOpen(const_cast<char*>(g_archives[a].c_str())) != 0)
            continue;

        struct ZipEntry* list = nullptr;
        INT32 count = 0;
        if (ZipGetList(&list, &count) != 0)
        {
            ZipClose();
            continue;
        }

        INT32 byCrc = -1, byName = -1;
        for (INT32 e = 0; e < count; e++)
        {
            if (list[e].nLen != ri.nLen) continue;
            if (byCrc < 0 && ri.nCrc != 0 && list[e].nCrc == ri.nCrc) { byCrc = e; break; }
            if (byName < 0 && !want.empty() && basename_lower(list[e].szName ? list[e].szName : "") == want)
                byName = e;
        }

        const INT32 entry = (byCrc >= 0) ? byCrc : byName;

        for (INT32 e = 0; e < count; e++) free(list[e].szName);
        free(list);

        if (entry < 0) { ZipClose(); continue; }

        INT32 wrote = 0;
        const INT32 loadRet = ZipLoadFile(dest, (INT32)ri.nLen, &wrote, entry);
        ZipClose();

        if (loadRet == 0 && wrote == (INT32)ri.nLen)
        {
            if (pnWrote) *pnWrote = wrote;
            return 0;
        }
    }

    if (pnWrote) *pnWrote = 0;
    if (getenv("RMGK_FBNEO_DEBUG_ROMS")) {
        fprintf(stderr, "[fbneo] ROM NOT FOUND: '%s' crc=%08x len=%u in %zu archive(s)\n",
                name ? name : "(null)", (unsigned)ri.nCrc, (unsigned)ri.nLen, g_archives.size());
    }
    return 1;
}

// Strip a single trailing archive extension: ZipOpen() re-appends ".zip"/".7z"
// to whatever it is given, so <rom>.zip must be handed over as "<rom>".
std::string archive_open_name(const std::string& path)
{
    std::string::size_type slash = path.find_last_of("/\\");
    std::string dir = slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
    std::string file = slash == std::string::npos ? path : path.substr(slash + 1);

    const char* exts[] = { ".zip", ".7z" };
    for (const char* e : exts)
    {
        if (file.size() > 4)
        {
            std::string lower;
            for (char c : file) lower.push_back((char)lower_c((unsigned char)c));
            std::string needle = e;
            if (lower.size() >= needle.size() &&
                lower.compare(lower.size() - needle.size(), needle.size(), needle) == 0)
            {
                file.erase(file.size() - needle.size());
                break;
            }
        }
    }
    return dir + file;
}

int find_driver_index(const std::string& gameName)
{
    if (gameName.empty()) return -1;

    int idx = BurnDrvGetIndex(const_cast<char*>(gameName.c_str()));
    if (idx >= 0) return idx;

    // The host may pass a zip file name ("sf2ce.zip") or a differently-cased
    // driver name. Walk the driver table comparing the game name to each
    // driver's short name.
    const std::string want = basename_lower(gameName);
    UINT32 oldActive = nBurnDrvActive;
    for (UINT32 i = 0; i < nBurnDrvCount; i++)
    {
        nBurnDrvActive = i;
        char* shortName = BurnDrvGetTextA(DRV_NAME);
        if (shortName && basename_lower(shortName) == want)
        {
            nBurnDrvActive = oldActive;
            return (int)i;
        }
    }
    nBurnDrvActive = oldActive;
    return -1;
}

void reset_all_digital_inputs()
{
    struct BurnInputInfo bii;
    for (UINT32 i = 0;; i++)
    {
        memset(&bii, 0, sizeof(bii));
        if (BurnDrvGetInputInfo(&bii, i) != 0) break;
        if (bii.nType == BIT_DIGITAL && bii.pVal) *bii.pVal = 0;
    }
}

void apply_input_masks(const std::vector<uint32_t>& masks)
{
    reset_all_digital_inputs();
    struct BurnInputInfo bii;
    std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
    for (size_t p = 0; p < masks.size() && p < g_playerInputMap.size(); p++)
    {
        const uint32_t m = masks[p];
        const auto& list = g_playerInputMap[p].targets;
        for (size_t b = 0; b < list.size() && b < 32; b++)
        {
            if (((m >> b) & 1u) == 0) continue;
            for (size_t t = 0; t < list[b].size(); t++)
            {
                memset(&bii, 0, sizeof(bii));
                if (BurnDrvGetInputInfo(&bii, list[b][t]) != 0) continue;
                if (bii.pVal) *bii.pVal = 1;
            }
        }
    }
}

void apply_latched_input()
{
    if (!g_inputCallback)
    {
        apply_input_masks(g_localMasks);
        return;
    }
    int players = g_inputPlayers > 0 ? g_inputPlayers : g_players;
    if ((int)g_latchedInput.size() < players * g_inputSize)
        g_latchedInput.assign((size_t)players * g_inputSize, 0);
    std::vector<uint32_t> masks((size_t)players, 0);
    memset(g_latchedInput.data(), 0, g_latchedInput.size());
    if (g_inputCallback(masks.data(), g_inputSize, players) == 0)
    {
        apply_input_masks(g_localMasks);
        return;
    }
    apply_input_masks(masks);
}

// FBNeo frontends normally turn the game's DIP switch list into constant
// inputs (GameInp) and load the per-switch defaults from the entries flagged
// with nFlags == 0xFF. This runtime writes the driver's input memory directly
// and never builds that table, so without this step every DIP byte (CPS1's
// Cpi01A/Cpi01C/Cpi01E, etc.) stays at zero -- which silently disables things
// like the CPS1 "Demo Sound" switch (attract mode is mute). Replicate the
// defaults here, writing them straight into the driver input memory.
void apply_dip_defaults()
{
    struct BurnDIPInfo bdi;
    int i;
    int nDIPOffset = 0;

    // The optional DIP_OFFSET marker shifts the indices of all later DIPs.
    for (i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
    {
        if (bdi.nFlags == 0xF0)
        {
            nDIPOffset = bdi.nInput;
            break;
        }
    }

    std::vector<std::pair<UINT32, UINT8> > dips;
    for (i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
    {
        if (bdi.nFlags != 0xFF) continue;   // 0xFF marks a DIP default entry

        const UINT32 idx = (UINT32)(bdi.nInput + nDIPOffset);
        const UINT8  bit = (UINT8)(bdi.nSetting & bdi.nMask);

        bool merged = false;
        for (size_t k = 0; k < dips.size(); k++)
        {
            if (dips[k].first == idx)
            {
                dips[k].second = (UINT8)((dips[k].second & ~bdi.nMask) | bit);
                merged = true;
                break;
            }
        }
        if (!merged) dips.push_back(std::make_pair(idx, bit));
    }

    for (size_t k = 0; k < dips.size(); k++)
    {
        struct BurnInputInfo bii;
        memset(&bii, 0, sizeof(bii));
        if (BurnDrvGetInputInfo(&bii, dips[k].first) != 0) continue;
        if (bii.pVal) *bii.pVal = dips[k].second;
    }

    // Re-apply any frontend overrides on top of the driver defaults so a
    // user's DIP choices survive a driver reset. Keys are BurnInputInfo
    // indices (already offset-adjusted, matching the defaults above).
    {
        std::vector<std::pair<int, uint8_t> > overrides;
        {
            std::lock_guard<std::mutex> lock(g_dipMutex);
            overrides = g_dipOverrides;
        }
        for (size_t k = 0; k < overrides.size(); k++)
        {
            struct BurnInputInfo bii;
            memset(&bii, 0, sizeof(bii));
            if (BurnDrvGetInputInfo(&bii, (UINT32)overrides[k].first) != 0) continue;
            if (bii.pVal) *bii.pVal = overrides[k].second;
        }
    }
}

// One ordered entry for a player: a display name plus the driver input indices
// it drives when active. Mirrors what the frontend shows and what the runtime
// mask bit selects.
struct DriverInputEntry { std::string name; std::vector<UINT32> targets; };

bool asciiEqualNoCase(const char* a, const char* b)
{
    for (; *a && *b; a++, b++)
    {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return false;
    }
    return *a == *b;
}

// Enumerates the driver currently selected in nBurnDrvActive and returns, per
// player, the plain digital inputs followed by the FBNeo-style combination
// macros. The order is the contract between the runtime mask (bit index), the
// bindings the frontend stores by name, and the input list it displays, so it
// must stay deterministic. Mirrors FinalBurn Neo's GameInpInitMacros().
void enumerate_input_entries(int maxPlayers, std::vector<std::vector<DriverInputEntry> >& out)
{
    out.assign((size_t)maxPlayers, std::vector<DriverInputEntry>());

    // Button slots used to synthesise the combos, per player.
    struct MacroSlots
    {
        int weakPunch = -1, mediumPunch = -1, strongPunch = -1;
        int weakKick  = -1, mediumKick  = -1, strongKick  = -1;
        int buttonA   = -1, buttonB     = -1, buttonC     = -1, buttonD = -1;
    };
    std::vector<MacroSlots> slots((size_t)maxPlayers);

    struct BurnInputInfo bii;
    for (UINT32 i = 0;; i++)
    {
        memset(&bii, 0, sizeof(bii));
        if (BurnDrvGetInputInfo(&bii, i) != 0) break;
        if (bii.nType != BIT_DIGITAL || !bii.szName) continue;
        if (bii.szName[0] != 'P' || bii.szName[2] != ' ' ||
            bii.szName[1] < '1' || bii.szName[1] > '9') continue;

        const int p = bii.szName[1] - '1';
        if (p < 0 || p >= maxPlayers) continue;
        if (out[(size_t)p].size() >= 32) continue;   // runtime mask is 32 bits

        DriverInputEntry entry;
        entry.name = bii.szName;
        entry.targets.push_back(i);
        out[(size_t)p].push_back(entry);

        const char* suffix = bii.szName + 2;
        MacroSlots& s = slots[(size_t)p];
        if      (asciiEqualNoCase(suffix, " Weak Punch"))   s.weakPunch   = (int)i;
        else if (asciiEqualNoCase(suffix, " Medium Punch")) s.mediumPunch = (int)i;
        else if (asciiEqualNoCase(suffix, " Strong Punch")) s.strongPunch = (int)i;
        else if (asciiEqualNoCase(suffix, " Weak Kick"))    s.weakKick    = (int)i;
        else if (asciiEqualNoCase(suffix, " Medium Kick"))  s.mediumKick  = (int)i;
        else if (asciiEqualNoCase(suffix, " Strong Kick"))  s.strongKick  = (int)i;
        else if (asciiEqualNoCase(suffix, " Button A"))     s.buttonA     = (int)i;
        else if (asciiEqualNoCase(suffix, " Button B"))     s.buttonB     = (int)i;
        else if (asciiEqualNoCase(suffix, " Button C"))     s.buttonC     = (int)i;
        else if (asciiEqualNoCase(suffix, " Button D"))     s.buttonD     = (int)i;
    }

    for (int p = 0; p < maxPlayers; p++)
    {
        const MacroSlots& s   = slots[(size_t)p];
        const std::string pfx = "P" + std::to_string(p + 1) + " Buttons ";

        auto addMacro = [&](const std::string& label, std::initializer_list<int> idx)
        {
            if (out[(size_t)p].size() >= 32) return;
            DriverInputEntry e;
            e.name = pfx + label;
            for (int v : idx)
            {
                if (v >= 0) e.targets.push_back((UINT32)v);
            }
            if (!e.targets.empty()) out[(size_t)p].push_back(e);
        };

        const bool punch3 = s.weakPunch >= 0 && s.mediumPunch >= 0 && s.strongPunch >= 0;
        const bool kick3  = s.weakKick  >= 0 && s.mediumKick  >= 0 && s.strongKick  >= 0;

        if (punch3) addMacro("3x Punch", { s.weakPunch,   s.mediumPunch, s.strongPunch });
        if (kick3)  addMacro("3x Kick",  { s.weakKick,    s.mediumKick,  s.strongKick  });
        if (punch3 && kick3)
        {
            addMacro("Weak PK",   { s.weakPunch,   s.weakKick   });
            addMacro("Medium PK", { s.mediumPunch, s.mediumKick });
            addMacro("Strong PK", { s.strongPunch, s.strongKick });
            addMacro("SP + WK",   { s.strongPunch, s.weakKick   });
        }

        if (s.buttonA >= 0 && s.buttonB >= 0 && s.buttonC >= 0 && s.buttonD >= 0)
        {
            addMacro("AB",   { s.buttonA, s.buttonB });
            addMacro("AC",   { s.buttonA, s.buttonC });
            addMacro("AD",   { s.buttonA, s.buttonD });
            addMacro("BC",   { s.buttonB, s.buttonC });
            addMacro("BD",   { s.buttonB, s.buttonD });
            addMacro("CD",   { s.buttonC, s.buttonD });
            addMacro("ABC",  { s.buttonA, s.buttonB, s.buttonC });
            addMacro("ABD",  { s.buttonA, s.buttonB, s.buttonD });
            addMacro("ACD",  { s.buttonA, s.buttonC, s.buttonD });
            addMacro("BCD",  { s.buttonB, s.buttonC, s.buttonD });
            addMacro("ABCD", { s.buttonA, s.buttonB, s.buttonC, s.buttonD });
        }
    }
}

void build_input_map()
{
    std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
    g_playerInputMap.assign((size_t)g_players, PlayerInputMap{});

    const int players = (g_players > 0) ? g_players : 2;
    std::vector<std::vector<DriverInputEntry> > entries;
    enumerate_input_entries(players, entries);

    for (int p = 0; p < (int)g_playerInputMap.size() && p < (int)entries.size(); p++)
    {
        for (size_t e = 0; e < entries[(size_t)p].size(); e++)
        {
            g_playerInputMap[(size_t)p].names.push_back(entries[(size_t)p][e].name);
            g_playerInputMap[(size_t)p].targets.push_back(entries[(size_t)p][e].targets);
        }
    }

    if (getenv("RMGK_FBNEO_TRACE") != nullptr)
    {
        for (int p = 0; p < (int)g_playerInputMap.size(); p++)
        {
            fprintf(stderr, "[FBNEO-IN] inputmap P%d count=%d:", p + 1,
                    (int)g_playerInputMap[(size_t)p].names.size());
            for (size_t e = 0; e < g_playerInputMap[(size_t)p].names.size(); e++)
            {
                fprintf(stderr, " [%d]%s", (int)e, g_playerInputMap[(size_t)p].names[e].c_str());
            }
            fprintf(stderr, "\n");
        }
    }
}

// ---------------------------------------------------------------------------
// DIP switch helpers.
//
// The driver exposes a flat table via BurnDrvGetDIPInfo. Entries are:
//   0xF0        -> offset marker (nInput = base index shift)
//   0xFE / 0xFD -> group header (szText = group name, nSetting = #settings)
//   0xFF        -> per-byte default value
//   other (>=1) -> a setting's first line (low nibble = #lines in the setting,
//                  0x40 = propagate writes to following lines, 0x80 = "ANY")
// A setting may span several lines; its continuation lines have nFlags == 0.
// Values live in the driver's input memory (BurnInputInfo[nInput + offset].pVal),
// which is exactly what apply_dip_defaults writes, so we read/write there too.

struct DipSettingRef { int firstLine; std::string name; };
struct DipGroupRef   { std::string name; int headerIndex; std::vector<DipSettingRef> settings; };

int dip_get_offset()
{
    struct BurnDIPInfo bdi;
    for (int i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
    {
        if (bdi.nFlags == 0xF0) return bdi.nInput;
    }
    return 0;
}

bool dip_enumerate(std::vector<DipGroupRef>& out)
{
    out.clear();
    if (!g_driverInited) return false;
    struct BurnDIPInfo bdi;
    int currentGroup = -1;
    for (int i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
    {
        if (bdi.nFlags == 0xF0 || bdi.nFlags == 0xFF) continue; // offset / default
        if (bdi.nFlags == 0xFE || bdi.nFlags == 0xFD)
        {
            DipGroupRef g;
            g.name        = bdi.szText ? bdi.szText : "";
            g.headerIndex = i;
            out.push_back(g);
            currentGroup  = (int)out.size() - 1;
            continue;
        }
        if (bdi.nFlags == 0) continue; // continuation line (skipped by its setting)
        if (currentGroup < 0) continue;
        DipSettingRef s;
        s.firstLine = i;
        s.name      = bdi.szText ? bdi.szText : "";
        out[currentGroup].settings.push_back(s);
        int lines = bdi.nFlags & 0x0F;
        if (lines > 1) i += (lines - 1); // skip this setting's continuation lines
    }
    return !out.empty();
}

bool dip_read_byte(int byteIdx, uint8_t& out)
{
    struct BurnInputInfo bii;
    memset(&bii, 0, sizeof(bii));
    if (BurnDrvGetInputInfo(&bii, (UINT32)byteIdx) != 0) return false;
    if (!bii.pVal) return false;
    out = *bii.pVal;
    return true;
}

bool dip_write_byte(int byteIdx, uint8_t value)
{
    struct BurnInputInfo bii;
    memset(&bii, 0, sizeof(bii));
    if (BurnDrvGetInputInfo(&bii, (UINT32)byteIdx) != 0) return false;
    if (!bii.pVal) return false;
    *bii.pVal = value;
    return true;
}

// Replica of inpdipsw.cpp CheckSetting(): is the setting whose first line is at
// idx the one currently selected, given the live driver input bytes?
bool dip_check_setting(int idx, int nDIPOffset)
{
    struct BurnDIPInfo bdi;
    if (BurnDrvGetDIPInfo(&bdi, idx) != 0) return false;
    uint8_t value = 0;
    if (!dip_read_byte(bdi.nInput + nDIPOffset, value)) return false;
    if ((value & bdi.nMask) != bdi.nSetting) return false;
    unsigned char nFlags = bdi.nFlags;
    if ((nFlags & 0x0F) <= 1) return true;
    for (int j = 1; j < (nFlags & 0x0F); j++)
    {
        struct BurnDIPInfo bdi2;
        if (BurnDrvGetDIPInfo(&bdi2, idx + j) != 0) return false;
        uint8_t v2 = 0;
        if (!dip_read_byte(bdi2.nInput + nDIPOffset, v2)) return false;
        if (nFlags & 0x80)
        {
            if ((v2 & bdi2.nMask) == bdi2.nSetting) return false;
        }
        else
        {
            if ((v2 & bdi2.nMask) != bdi2.nSetting) return false;
        }
    }
    return true;
}

// Replica of inpdipsw.cpp write handler: RMW the setting's first-line byte and,
// when 0x40 is set, propagate to the following nFlags == 0 lines. Collects every
// byte written (index -> value) into *written so the caller can record overrides.
bool dip_select_setting(int idx, int nDIPOffset, std::vector<std::pair<int, uint8_t> >* written)
{
    struct BurnDIPInfo bdi;
    if (BurnDrvGetDIPInfo(&bdi, idx) != 0) return false;
    int byteIdx = bdi.nInput + nDIPOffset;
    uint8_t cur = 0;
    if (!dip_read_byte(byteIdx, cur)) return false;
    uint8_t next = (uint8_t)((cur & ~bdi.nMask) | (bdi.nSetting & bdi.nMask));
    if (!dip_write_byte(byteIdx, next)) return false;
    if (written) written->push_back(std::make_pair(byteIdx, next));
    if (bdi.nFlags & 0x40)
    {
        struct BurnDIPInfo b2;
        for (int j = idx + 1; BurnDrvGetDIPInfo(&b2, j) == 0; j++)
        {
            if (b2.nFlags != 0) break;
            int bIdx = b2.nInput + nDIPOffset;
            uint8_t c2 = 0;
            if (!dip_read_byte(bIdx, c2)) return false;
            uint8_t n2 = (uint8_t)((c2 & ~b2.nMask) | (b2.nSetting & b2.nMask));
            if (!dip_write_byte(bIdx, n2)) return false;
            if (written) written->push_back(std::make_pair(bIdx, n2));
        }
    }
    return true;
}

} // namespace

bool Initialize(const char* gameName, const char* const* archives, int archiveCount, int players)
{
    if (g_driverInited) return true;

    g_paused.store(false, std::memory_order_relaxed);

    if (!ensure_lib_init())
    {
        return false;
    }

    g_players = (players > 0) ? players : 2;
    {
        std::lock_guard<std::mutex> lock(g_localInputMutex);
        g_localMasks.assign((size_t)g_players, 0);
    }
    g_inputSize = 4;
    g_latchedInput.assign((size_t)g_players * g_inputSize, 0);
    g_driverName = gameName ? gameName : "";

    g_archives.clear();
    g_archiveFiles.clear();
    if (archives) {
        for (int i = 0; i < archiveCount && archives[i]; i++)
        {
            // Skip archives that do not exist: the driver may legitimately only
            // ship part of a romset (parent zips are supplied separately).
            std::error_code ec;
            if (!std::filesystem::exists(archives[i], ec)) continue;
            g_archiveFiles.emplace_back(archives[i]);
            g_archives.emplace_back(archive_open_name(archives[i]));
        }
    }

    if (!g_driverName.empty())
    {
        int idx = find_driver_index(g_driverName);
        if (idx < 0)
        {
            CoreSetError("FBNeo initialization failed: unknown driver \"" + g_driverName + "\"");
            fprintf(stderr, "[fbneo] driver not found: '%s'\n", g_driverName.c_str());
            Shutdown();
            return false;
        }
        nBurnDrvActive = (UINT32)idx;
    }

    nMaxPlayers = BurnDrvGetMaxPlayers();
    nBurnLayer  = 0xFF;
    nBurnSoundRate = 48000;
    nBurnSoundLen  = 0;
    pBurnSoundOut = nullptr;

    // A generous default framebuffer; drivers that draw during BurnDrvInit()
    // have somewhere to write. It is resized to the real geometry afterwards.
    g_videoBuffer.assign(1024ull * 1024ull * 4ull, 0);
    nBurnBpp   = 4;
    nBurnPitch = 1024 * 4;
    pBurnDraw  = g_videoBuffer.data();

    BurnHighCol = fbneo_highcol32;

    BurnExtLoadRom = fbneo_load_rom;

    if (BurnDrvInit() != 0)
    {
        CoreSetError("FBNeo initialization failed: BurnDrvInit failed for \"" + g_driverName + "\" (missing or incomplete romset/BIOS files?)");
        fprintf(stderr, "[fbneo] BurnDrvInit failed for '%s' (%zu archive(s)):\n", g_driverName.c_str(), g_archives.size());
        for (const auto& a : g_archiveFiles) fprintf(stderr, "[fbneo]   archive: %s\n", a.c_str());
        BurnExtLoadRom = nullptr;
        Shutdown();
        return false;
    }

    // The lib frontend has no video plugin to trigger this, so palette entries
    // that were computed during BurnDrvInit() (and the driver's cached palette
    // LUT, e.g. CPS' CpsPal) still hold the white filler output. Signal the
    // driver to rebuild its whole palette with the converter installed above.
    BurnRecalcPal();

    // BurnDrvInit()/reset blanks the frontend-owned DIP bytes; restore the
    // driver's declared defaults before the first frame runs.
    apply_dip_defaults();

    INT32 fw = 0, fh = 0;
    // Full framebuffer. Drivers can render well outside the visible window: the
    // CPS1 renderer, for instance, uses a 384x1024 buffer while only the top 224
    // lines are ever on screen.
    BurnDrvGetFullSize(&fw, &fh);
    if (fw <= 0 || fh <= 0) BurnDrvGetVisibleSize(&fw, &fh);
    if (fw <= 0 || fh <= 0) { fw = 384; fh = 224; }

    g_orientationFlags = 0;
    {
        const INT32 flags = BurnDrvGetFlags();
        if (flags & BDF_ORIENTATION_VERTICAL) g_orientationFlags |= kVideoVertical;
        if (flags & BDF_ORIENTATION_FLIPPED)  g_orientationFlags |= kVideoFlipped;
    }

    // Visible region shown to the user. For horizontal drivers it is the
    // top-left vw x vh rectangle of the full buffer (BurnDrvGetVisibleOffs is
    // 0,0); vertical drivers report the already-rotated full size as displayed.
    INT32 vw = fw, vh = fh;
    if (!(g_orientationFlags & kVideoVertical))
    {
        INT32 cvw = 0, cvh = 0;
        BurnDrvGetVisibleSize(&cvw, &cvh);
        if (cvw > 0 && cvh > 0 && cvw <= fw && cvh <= fh) { vw = cvw; vh = cvh; }
    }

    // Re-allocating can move the buffer, so re-point pBurnDraw afterwards.
    g_videoBuffer.assign((size_t)fw * fh * 4, 0);
    nBurnPitch = fw * 4;
    pBurnDraw  = g_videoBuffer.data();
    g_videoFrame.pixels = g_videoBuffer.data();
    g_videoFrame.width  = vw;
    g_videoFrame.height = vh;
    g_videoFrame.pitch  = fw * 4;   // stride stays at the full buffer width
    g_videoFrame.bpp    = 4;

    if (nBurnFPS > 0)
        nBurnSoundLen = (nBurnSoundRate * 100 + (nBurnFPS >> 1)) / nBurnFPS;
    g_soundFrames = nBurnSoundLen;
    g_soundBuffer.assign((size_t)(nBurnSoundLen > 0 ? nBurnSoundLen : 1) * 2, 0);
    pBurnSoundOut = g_soundBuffer.data();

    build_input_map();

    g_driverInited = true;

    // The mupen64plus core isn't used for arcade sessions, but front-end code
    // (e.g. CoreGetCurrentRomSettings) may still have latched an INVALID_STATE
    // error before we got here. The runtime is ready now, so drop it.
    CoreSetError(std::string());
    return true;
}

bool IsInitialized() { return g_driverInited; }

const char* DriverShortName() { return g_driverName.c_str(); }

bool Reset()
{
    if (!g_driverInited) return false;

    // Reset Game is an exit + re-init of the driver, which preserves the
    // selected driver and the already-resolved archives. That teardown must not
    // run underneath a live frame or the presenter, so defer it to the
    // emulation thread (applied at the top of the next RunFrame under the frame
    // lock). Requesting it from the GUI thread this way is race-free.
    g_pendingReset.store(true, std::memory_order_relaxed);
    return true;
}

// Applies a deferred reset. Caller must hold g_frameMutex.
static bool apply_pending_reset()
{
    BurnDrvExit();
    g_driverInited = false;

    if (BurnDrvInit() != 0)
    {
        CoreSetError("FBNeo reset failed: BurnDrvInit failed for \"" + g_driverName + "\"");
        return false;
    }

    BurnRecalcPal();
    apply_dip_defaults();
    g_driverInited = true;
    return true;
}

// Look up a driver by short name or archive stem (e.g. "sf2ce" / "sf2ce.zip").
// Fills *outFull with the driver's full display name when requested. The cache
// is populated lazily on the first call by walking the driver table once.
static bool driver_lookup(const char* gameName, std::string* outFull)
{
    if (!gameName || !*gameName) return false;

    std::lock_guard<std::mutex> lock(g_driverNameMutex);

    if (g_driverNameCache.empty())
    {
        if (!ensure_lib_init())
        {
            return false;
        }

        UINT32 oldActive = nBurnDrvActive;
        for (UINT32 i = 0; i < nBurnDrvCount; i++)
        {
            nBurnDrvActive = i;
            const char* shortName = BurnDrvGetTextA(DRV_NAME);
            const char* full      = BurnDrvGetTextA(DRV_FULLNAME);
            if (shortName && *shortName)
                g_driverNameCache[basename_lower(shortName)] =
                    (full && *full) ? std::string(full) : std::string(shortName);
        }
        nBurnDrvActive = oldActive;
    }

    auto it = g_driverNameCache.find(basename_lower(gameName));
    if (it == g_driverNameCache.end()) return false;
    if (outFull) *outFull = it->second;
    return true;
}

bool IsDriver(const char* gameName)
{
    return driver_lookup(gameName, nullptr);
}

bool DriverFullName(const char* gameName, std::string& outName)
{
    return driver_lookup(gameName, &outName);
}

bool DriverRomId(const char* gameName, std::string& outHex)
{
    if (!gameName || !*gameName) return false;
    outHex = driver_rom_id(basename_lower(gameName));
    return true;
}

void Shutdown()
{
    if (g_driverInited)
    {
        BurnDrvExit();
        g_driverInited = false;
    }
    g_paused.store(false, std::memory_order_relaxed);
    BurnExtLoadRom = nullptr;
    pBurnDraw      = nullptr;
    pBurnSoundOut  = nullptr;
    g_videoFrame   = {};
    g_videoBuffer.clear();
    g_soundBuffer.clear();
    {
        std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
        g_playerInputMap.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_localInputMutex);
        g_localMasks.clear();
    }
    g_latchedInput.clear();
    g_archives.clear();
    g_archiveFiles.clear();
    // BurnLibExit() is intentionally NOT called here: BurnLibInit() calls it
    // internally, so a per-session exit would double-free the driver-name tables
    // on the next game (see ensure_lib_init()). The coarse tables stay alive for
    // the process and are released once via atexit.
}

int PlayerCount()       { return g_players; }
int InputBytesPerPlayer(){ return g_inputSize; }

void SetLocalInput(int playerIndex, uint32_t mask)
{
    std::lock_guard<std::mutex> lock(g_localInputMutex);
    if (playerIndex >= 0 && playerIndex < (int)g_localMasks.size())
        g_localMasks[playerIndex] = mask;
}

uint32_t GetLocalInput(int playerIndex)
{
    std::lock_guard<std::mutex> lock(g_localInputMutex);
    if (playerIndex >= 0 && playerIndex < (int)g_localMasks.size())
        return g_localMasks[playerIndex];
    return 0;
}

int GetPlayerInputCount(int playerIndex)
{
    std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
    if (playerIndex < 0 || playerIndex >= (int)g_playerInputMap.size())
        return 0;
    return (int)g_playerInputMap[playerIndex].names.size();
}

const char* GetPlayerInputName(int playerIndex, int bit)
{
    std::lock_guard<std::mutex> lock(g_playerInputMapMutex);
    if (playerIndex < 0 || playerIndex >= (int)g_playerInputMap.size())
        return nullptr;
    const auto& names = g_playerInputMap[playerIndex].names;
    if (bit < 0 || bit >= (int)names.size())
        return nullptr;
    return names[bit].c_str();
}

bool DriverInputNames(const char* gameName, std::vector<std::vector<std::string> >& outPerPlayer)
{
    outPerPlayer.clear();
    if (!gameName || !*gameName)
        return false;

    // Only meaningful outside a live session; the runtime's own input map is
    // authoritative while a driver is running.
    if (g_driverInited)
        return false;

    if (!ensure_lib_init())
    {
        return false;
    }

    const int idx = find_driver_index(gameName);
    if (idx < 0)
        return false;

    const int maxPlayers = 4;
    outPerPlayer.assign((size_t)maxPlayers, std::vector<std::string>());

    UINT32 oldActive = nBurnDrvActive;
    nBurnDrvActive = (UINT32)idx;

    // Same enumeration the runtime uses, so the names shown here (including the
    // combination macros) are exactly the ones the frontend can bind.
    std::vector<std::vector<DriverInputEntry> > entries;
    enumerate_input_entries(maxPlayers, entries);
    for (size_t p = 0; p < entries.size() && p < outPerPlayer.size(); p++)
    {
        for (size_t e = 0; e < entries[p].size(); e++)
        {
            outPerPlayer[p].push_back(entries[p][e].name);
        }
    }

    nBurnDrvActive = oldActive;

    return true;
}

void SetPaused(bool paused)
{
    g_paused.store(paused, std::memory_order_relaxed);
}

bool IsPaused()
{
    return g_paused.load(std::memory_order_relaxed);
}

int GetDipGroupCount()
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    std::vector<DipGroupRef> groups;
    if (!dip_enumerate(groups)) return 0;
    return (int)groups.size();
}

bool GetDipGroup(int group, DipGroupInfo& out)
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    std::vector<DipGroupRef> groups;
    if (!dip_enumerate(groups)) return false;
    if (group < 0 || group >= (int)groups.size()) return false;
    const DipGroupRef& g = groups[group];
    int nDIPOffset = dip_get_offset();
    out.name          = g.name;
    out.settingCount  = (int)g.settings.size();
    out.selectedSetting = -1;
    for (int s = 0; s < (int)g.settings.size(); s++)
    {
        if (dip_check_setting(g.settings[s].firstLine, nDIPOffset))
        {
            out.selectedSetting = s;
            break;
        }
    }
    return true;
}

bool GetDipSettingName(int group, int setting, std::string& outName)
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    std::vector<DipGroupRef> groups;
    if (!dip_enumerate(groups)) return false;
    if (group < 0 || group >= (int)groups.size()) return false;
    const DipGroupRef& g = groups[group];
    if (setting < 0 || setting >= (int)g.settings.size()) return false;
    const std::string& settingName = g.settings[setting].name;
    if (!g.name.empty())
        outName = g.name + ": " + settingName;
    else
        outName = settingName;
    return true;
}

bool SetDipSelection(int group, int setting)
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    std::vector<DipGroupRef> groups;
    if (!dip_enumerate(groups)) return false;
    if (group < 0 || group >= (int)groups.size()) return false;
    const DipGroupRef& g = groups[group];
    if (setting < 0 || setting >= (int)g.settings.size()) return false;
    int nDIPOffset = dip_get_offset();
    std::vector<std::pair<int, uint8_t> > written;
    if (!dip_select_setting(g.settings[setting].firstLine, nDIPOffset, &written))
        return false;
    // Remember the written bytes so they survive a driver reset.
    if (!written.empty())
    {
        std::lock_guard<std::mutex> dlock(g_dipMutex);
        for (size_t k = 0; k < written.size(); k++)
        {
            bool merged = false;
            for (size_t m = 0; m < g_dipOverrides.size(); m++)
            {
                if (g_dipOverrides[m].first == written[k].first)
                {
                    g_dipOverrides[m].second = written[k].second;
                    merged = true;
                    break;
                }
            }
            if (!merged) g_dipOverrides.push_back(written[k]);
        }
    }
    return true;
}

void ResetDipsToDefaults()
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    {
        std::lock_guard<std::mutex> dlock(g_dipMutex);
        g_dipOverrides.clear();
    }
    apply_dip_defaults();
}

void SetDipOverrides(const std::vector<std::pair<int, uint8_t> >& overrides)
{
    std::lock_guard<std::mutex> lock(g_dipMutex);
    g_dipOverrides = overrides;
}

void GetDipOverrides(std::vector<std::pair<int, uint8_t> >& out)
{
    std::lock_guard<std::mutex> lock(g_dipMutex);
    out = g_dipOverrides;
}

bool SaveStateInto(unsigned char* buffer, int capacity, int& outLen, int& outChecksum, int /*frame*/)
{
    if (!g_driverInited || !buffer || capacity <= 0) return false;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    UINT8* def = nullptr;
    INT32  len = 0;
    if (BurnStateCompress(&def, &len, 1) != 0 || !def || len <= 0)
    {
        if (def) free(def);
        return false;
    }
    if (len > capacity) { free(def); return false; }
    memcpy(buffer, def, (size_t)len);
    outLen      = len;
    outChecksum = (int)fnv1a(def, (size_t)len);
    free(def);
    return true;
}

bool SaveStateAlloc(unsigned char*& outBuffer, int& outLen, int& outChecksum, int /*frame*/)
{
    if (!g_driverInited) return false;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    UINT8* def = nullptr;
    INT32  len = 0;
    if (BurnStateCompress(&def, &len, 1) != 0 || !def || len <= 0)
    {
        if (def) free(def);
        return false;
    }
    outBuffer  = (unsigned char*)malloc((size_t)len);
    if (!outBuffer) { free(def); return false; }
    memcpy(outBuffer, def, (size_t)len);
    free(def);
    outLen      = len;
    outChecksum = (int)fnv1a(outBuffer, (size_t)len);
    return true;
}

bool LoadState(const unsigned char* buffer, int len)
{
    if (!g_driverInited || !buffer || len <= 0) return false;
    std::lock_guard<std::mutex> lock(g_frameMutex);
    // Rollback resim loads re-run already-audible frames and the sound chips
    // re-trigger. Snapshot ONLY the sound state before the load restores the
    // pre-rollback emu state; the next audible frame replays it (video/logic
    // keep the corrected state).
    if (!rmgkSndPending)
    {
        rmgkSndPending = rmgkSndArm();
    }
    return BurnStateDecompress((UINT8*)buffer, len, 1) == 0;
}

// TEMPORARY debug aid: dumps the current visible frame to a PPM (P6) when
// RMGK_FBNEO_DUMP_FRAME=<path> is set, once, a few frames in. Used to verify
// the emulated video output without a display.
static void debug_maybe_dump_frame()
{
    static bool s_dumped = false;
    static int  s_counter = 0;
    if (s_dumped) return;
    const char* path = std::getenv("RMGK_FBNEO_DUMP_FRAME");
    if (!path || !*path) return;
    if (++s_counter < 120) return;
    if (s_counter != 120 && s_counter != 600 && s_counter != 1200) return;
    if (s_counter == 1200) s_dumped = true;

    char pathBuf[1024];
    std::snprintf(pathBuf, sizeof(pathBuf), "%s.%d", path, s_counter);
    FILE* f = fopen(pathBuf, "wb");
    if (!f) return;
    const int w = g_videoFrame.width, h = g_videoFrame.height;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
    {
        const uint8_t* row = g_videoFrame.pixels + (size_t)y * g_videoFrame.pitch;
        for (int x = 0; x < w; x++)
        {
            const uint8_t b = row[x * 4 + 0];
            const uint8_t g = row[x * 4 + 1];
            const uint8_t r = row[x * 4 + 2];
            fputc(r, f); fputc(g, f); fputc(b, f);
        }
    }
    fclose(f);
    fprintf(stderr, "[FBNEO-DBG] dumped frame %dx%d to %s\n", w, h, pathBuf); fflush(stderr);
}

bool RunFrame(int flags)
{
    if (!g_driverInited) return false;
    std::lock_guard<std::mutex> lock(g_frameMutex);

    if (g_pendingReset.exchange(false, std::memory_order_relaxed))
    {
        if (!apply_pending_reset())
        {
            return false;
        }
    }

    // Paused: keep the presenter fed with the last rendered frame and silence,
    // but don't step the driver. The reset above still runs so Reset-while-
    // paused works. The rollback/pacing loop keeps calling us, so the window
    // never goes black and timing stays stable.
    if (g_paused.load(std::memory_order_relaxed))
    {
        if (flags & Output_Audio) {
            const size_t bytes = (size_t)(nBurnSoundLen > 0 ? nBurnSoundLen : 0) * 2 * sizeof(int16_t);
            memset(g_soundBuffer.data(), 0, std::min(bytes, g_soundBuffer.size() * sizeof(int16_t)));
        }
        return true;
    }

    const bool audioOutput = (flags & Output_Audio) != 0;
    // First audible frame after a rollback resim batch: put the sound chips
    // back on the audible timeline so resim-retriggered voices are dropped.
    if (audioOutput)
    {
        rmgkSndMaybeReplay();
        rmgkSndAudioFramesEmitted++;
    }

    apply_latched_input();
    pBurnDraw     = (flags & Output_Video) ? g_videoBuffer.data() : nullptr;
    pBurnSoundOut = (flags & Output_Audio) ? g_soundBuffer.data() : nullptr;

    // The sound cores accumulate into pBurnSoundOut instead of overwriting it,
    // so the buffer has to be zeroed before each frame -- otherwise the samples
    // pile up frame after frame and eventually overflow. (Every other FBNeo
    // frontend does this; omitting it made replayed frames diverge.)
    if (flags & Output_Audio) {
        const size_t bytes = (size_t)(nBurnSoundLen > 0 ? nBurnSoundLen : 0) * 2 * sizeof(int16_t);
        memset(g_soundBuffer.data(), 0, std::min(bytes, g_soundBuffer.size() * sizeof(int16_t)));
    }

    auto t0 = clock_::now();
    BurnDrvFrame();
    g_stats = {};
    g_stats.total_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        clock_::now() - t0).count();
    return true;
}

bool Execute(const m64p_rollback_execute_callbacks& callbacks)
{
    if (!g_driverInited) return false;
    for (;;)
    {
        int r = callbacks.begin_frame ? callbacks.begin_frame(callbacks.user_data) : 0;
        if (r != 1) break;
        if (callbacks.pace_before_present)
            callbacks.pace_before_present(callbacks.user_data);
        rmgk_gekko_trace_loop_mark(3);
        RunFrame(Output_Video | Output_Audio);
        rmgk_gekko_trace_loop_mark(4);
        debug_maybe_dump_frame();
        if (g_presentCallback)
        {
            rmgk_gekko_trace_loop_mark(5);
            g_presentCallback(g_videoFrame.pixels, g_videoFrame.width, g_videoFrame.height,
                              g_videoFrame.pitch, g_orientationFlags,
                              g_soundBuffer.data(), g_soundFrames, g_presentUser);
            rmgk_gekko_trace_loop_mark(6);
        }
        if (callbacks.visible_frame_complete)
            callbacks.visible_frame_complete(callbacks.user_data, &g_stats);
        if (callbacks.end_frame && callbacks.end_frame(callbacks.user_data) == 0)
            return false;
    }
    return true;
}

bool SampleInput(void* values, int size, int players)
{
    if (!values || size != g_inputSize) return false;
    uint8_t* v = (uint8_t*)values;
    for (int i = 0; i < players; i++)
    {
        uint32_t m = 0;
        {
            std::lock_guard<std::mutex> lock(g_localInputMutex);
            m = (i < (int)g_localMasks.size()) ? g_localMasks[i] : 0;
        }
        memcpy(v + (size_t)i * size, &m, (size_t)size);
    }
    return true;
}

void SetInputCallback(int (*callback)(void* values, int size, int players))
{
    g_inputCallback = callback;
}

void SetInputPlayers(int players)     { g_inputPlayers = players; }

void SetNetgameMode(bool netgame)
{
    // Pins/clears the NeoGeo RTC + BurnRandom seed inside the core. Meaningful
    // only before Initialize(); changing it mid-session has no effect on the
    // already-seeded drivers.
    nRMGKNetgameMode = netgame ? 1 : 0;
}

void SetDeterministic(bool enabled)   { (void)enabled; }
void SetTimesyncScale(double scale)   { (void)scale; }
void SetVerboseStats(bool enabled)    { (void)enabled; }

void SetFrameCallback(FrameCallback callback, void* user)
{
    g_presentCallback = callback;
    g_presentUser     = user;
}

const VideoFrame& GetVideoFrame()     { return g_videoFrame; }

const int16_t* GetSoundBuffer(int& sampleFrames)
{
    sampleFrames = g_soundFrames;
    return g_soundBuffer.data();
}

void GetRunFrameStats(m64p_rollback_run_frame_stats& out) { out = g_stats; }

} // namespace fbneo_rt
