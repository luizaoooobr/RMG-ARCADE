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
#include "FbneoRuntime.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
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

// ZipLoadOneFile lives in src/burner/zipfn.cpp, which the lib frontend links in.
// Same story as above: C++ linkage, so no extern "C". It fills a caller-provided
// buffer from a zip/7z archive, which is exactly what BurnExtLoadRom needs --
// that is how this runtime reads ROMs without pulling in RMG's minizip layer.
extern INT32 ZipLoadOneFile(char* arcName, const char* fileName, void** Dest, INT32* pnWrote);

namespace fbneo_rt
{
namespace
{
using clock_ = std::chrono::steady_clock;

bool         g_libInited     = false;
bool         g_driverInited  = false;
int          g_players       = 2;
std::vector<uint32_t> g_localMasks;

int (*g_inputCallback)(void*, int, int) = nullptr;
int  g_inputPlayers  = 0;
int  g_inputSize     = 4;
std::vector<uint8_t> g_latchedInput;

// Archives that make up the romset. g_archives holds the *open name* (extension
// stripped, because ZipOpen() re-appends it); g_archiveFiles keeps the original
// path for diagnostics.
std::vector<std::string> g_archives;
std::vector<std::string> g_archiveFiles;
std::string              g_driverName;

struct PlayerInputMap { std::vector<int> digitalIndices; };
std::vector<PlayerInputMap> g_playerInputMap;

std::vector<uint8_t>   g_videoBuffer;
VideoFrame             g_videoFrame;

std::vector<int16_t>   g_soundBuffer;
int                    g_soundFrames = 0;

m64p_rollback_run_frame_stats g_stats = {};

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

// FBNeo's BurnLoadRomExt() asks the application to decode ROM index i into the
// caller's Dest buffer, sized to the driver's declared length. Match the file
// by basename inside each archive.
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
    if (ri.nType == 0 || ri.nLen <= 0) { if (pnWrote) *pnWrote = 0; return 1; }

    char* name = nullptr;
    BurnDrvGetRomName(&name, i, 0);
    if (!name || !*name) { if (pnWrote) *pnWrote = 0; return 1; }

    // ZipLoadOneFile matches against the whole archive entry name, so retry with
    // just the basename when the driver asks for a path-qualified name.
    const std::string bare = basename_lower(name);

    for (size_t a = 0; a < g_archives.size(); a++)
    {
        const char* candidates[3] = { name, bare.c_str(), nullptr };
        for (int c = 0; c < 2; c++)
        {
            if (!candidates[c] || !*candidates[c]) continue;
            void* p = static_cast<void*>(dest);
            INT32 wrote = 0;
            if (ZipLoadOneFile(const_cast<char*>(g_archives[a].c_str()), candidates[c], &p, &wrote) == 0 &&
                p == static_cast<void*>(dest))
            {
                if (pnWrote) *pnWrote = wrote;
                return 0;
            }
        }
    }

    if (pnWrote) *pnWrote = 0;
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
    for (size_t p = 0; p < masks.size() && p < g_playerInputMap.size(); p++)
    {
        const uint32_t m = masks[p];
        const auto& list = g_playerInputMap[p].digitalIndices;
        for (size_t b = 0; b < list.size() && b < 32; b++)
        {
            memset(&bii, 0, sizeof(bii));
            if (BurnDrvGetInputInfo(&bii, (UINT32)list[b]) != 0) continue;
            if (bii.pVal) *bii.pVal = ((m >> b) & 1u) ? 1 : 0;
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

void build_input_map()
{
    g_playerInputMap.assign((size_t)g_players, PlayerInputMap{});
    struct BurnInputInfo bii;
    for (UINT32 i = 0;; i++)
    {
        memset(&bii, 0, sizeof(bii));
        if (BurnDrvGetInputInfo(&bii, i) != 0) break;
        if (bii.nType != BIT_DIGITAL || !bii.szName) continue;
        if (bii.szName[0] == 'P' && bii.szName[2] == ' ' &&
            bii.szName[1] >= '1' && bii.szName[1] <= '9')
        {
            int p = bii.szName[1] - '1';
            if (p >= 0 && p < g_players)
                g_playerInputMap[p].digitalIndices.push_back((int)i);
        }
    }
}

} // namespace

bool Initialize(const char* gameName, const char* const* archives, int archiveCount, int players)
{
    if (g_driverInited) return true;

    if (!g_libInited)
    {
        if (BurnLibInit() != 0) return false;
        g_libInited = true;
    }

    g_players = (players > 0) ? players : 2;
    g_localMasks.assign((size_t)g_players, 0);
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
        fprintf(stderr, "DBGC idx=");
        if (idx < 0)
        {
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

    BurnExtLoadRom = fbneo_load_rom;
    fprintf(stderr, "DBGD
");

    fprintf(stderr, "DBGDBEGIN\n");
    if (BurnDrvInit() != 0)
    {
        BurnExtLoadRom = nullptr;
        Shutdown();
        return false;
    }

    INT32 fw = 0, fh = 0;
    BurnDrvGetFullSize(&fw, &fh);
    if (fw <= 0 || fh <= 0) BurnDrvGetVisibleSize(&fw, &fh);
    if (fw <= 0 || fh <= 0) fw = 1024; fh = 1024;

    // Re-allocating can move the buffer, so re-point pBurnDraw afterwards.
    g_videoBuffer.assign((size_t)fw * fh * 4, 0);
    nBurnPitch = fw * 4;
    pBurnDraw  = g_videoBuffer.data();
    g_videoFrame.pixels = g_videoBuffer.data();
    g_videoFrame.width  = fw;
    g_videoFrame.height = fh;
    g_videoFrame.pitch  = fw * 4;
    g_videoFrame.bpp    = 4;

    if (nBurnFPS > 0)
        nBurnSoundLen = (nBurnSoundRate * 100 + (nBurnFPS >> 1)) / nBurnFPS;
    g_soundFrames = nBurnSoundLen;
    g_soundBuffer.assign((size_t)(nBurnSoundLen > 0 ? nBurnSoundLen : 1) * 2, 0);
    pBurnSoundOut = g_soundBuffer.data();

    build_input_map();
    fprintf(stderr, "DBGF
");

    g_driverInited = true;
    return true;
}

bool IsInitialized() { return g_driverInited; }

void Shutdown()
{
    if (g_driverInited)
    {
        BurnDrvExit();
        g_driverInited = false;
    }
    BurnExtLoadRom = nullptr;
    pBurnDraw      = nullptr;
    pBurnSoundOut  = nullptr;
    g_videoFrame   = {};
    g_videoBuffer.clear();
    g_soundBuffer.clear();
    g_playerInputMap.clear();
    g_localMasks.clear();
    g_latchedInput.clear();
    g_archives.clear();
    g_archiveFiles.clear();
    if (g_libInited)
    {
        BurnLibExit();
        g_libInited = false;
    }
}

int PlayerCount()       { return g_players; }
int InputBytesPerPlayer(){ return g_inputSize; }

void SetLocalInput(int playerIndex, uint32_t mask)
{
    if (playerIndex >= 0 && playerIndex < g_players)
        g_localMasks[playerIndex] = mask;
}

uint32_t GetLocalInput(int playerIndex)
{
    if (playerIndex >= 0 && playerIndex < g_players)
        return g_localMasks[playerIndex];
    return 0;
}

bool SaveStateInto(unsigned char* buffer, int capacity, int& outLen, int& outChecksum, int /*frame*/)
{
    if (!g_driverInited || !buffer || capacity <= 0) return false;
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
    return BurnStateDecompress((UINT8*)buffer, len, 1) == 0;
}

bool RunFrame(int flags)
{
    if (!g_driverInited) return false;
    apply_latched_input();
    pBurnDraw     = (flags & Output_Video) ? g_videoBuffer.data() : nullptr;
    pBurnSoundOut = (flags & Output_Audio) ? g_soundBuffer.data() : nullptr;
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
        RunFrame(Output_Video | Output_Audio);
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
        uint32_t m = (i < g_players) ? g_localMasks[i] : 0;
        memcpy(v + (size_t)i * size, &m, (size_t)size);
    }
    return true;
}

void SetInputCallback(int (*callback)(void* values, int size, int players))
{
    g_inputCallback = callback;
}

void SetInputPlayers(int players)     { g_inputPlayers = players; }
void SetDeterministic(bool enabled)   { (void)enabled; }
void SetTimesyncScale(double scale)   { (void)scale; }
void SetVerboseStats(bool enabled)    { (void)enabled; }

const VideoFrame& GetVideoFrame()     { return g_videoFrame; }

const int16_t* GetSoundBuffer(int& sampleFrames)
{
    sampleFrames = g_soundFrames;
    return g_soundBuffer.data();
}

void GetRunFrameStats(m64p_rollback_run_frame_stats& out) { out = g_stats; }

} // namespace fbneo_rt

