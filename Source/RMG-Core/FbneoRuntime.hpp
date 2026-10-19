/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_FBNEO_RUNTIME_HPP
#define CORE_FBNEO_RUNTIME_HPP

// FinalBurn Neo embedding runtime.
//
// This is the RMG-K side glue for the headless FBNeo "lib" core
// (opencode/fbneo_core). It mirrors the rollback command surface that
// RMG-Core used to drive through the patched mupen64plus core
// (M64CMD_ROLLBACK_*), but backed by FBNeo's BurnDrv* API.

#include "m64p/api/m64p_types.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fbneo_rt
{
    // Frame output flags (matches CoreFrameOutputFlags semantics).
    enum FrameOutputFlags
    {
        Output_None  = 0,
        Output_Video = 1 << 0,
        Output_Audio = 1 << 1,
    };

    struct VideoFrame
    {
        const uint8_t* pixels = nullptr;
        int width  = 0;
        int height = 0;
        int pitch  = 0; // bytes per line
        int bpp    = 0; // bytes per pixel (2 or 4)
    };

    // Lifecycle.
    //
    // gameName  : FBNeo driver short name or zip name used to auto-select the driver.
    // archives  : zero or more zip archives that hold the romset.
    // players   : number of netplay seats (input masks are maintained per seat).
    bool Initialize(const char* gameName, const char* const* archives, int archiveCount, int players);
    bool IsInitialized();
    void Shutdown();

    // Reset the active driver (FBNeo's "Reset Game"): tears it down and
    // re-initializes it, keeping the current driver/archives. Thread-safe with
    // respect to the frame loop.
    bool Reset();

    // Short driver name of the active session (e.g. "kof98"); empty when no
    // session is running. Used by the save-state path to name the state file.
    const char* DriverShortName();

    // Driver probe (used by the loading path / ROM browser to tell an FBNeo
    // romset from an N64 archive). Initializes the Burn library on first use.
    bool IsDriver(const char* gameName);
    // Full (human readable) driver name, e.g. "Street Fighter II' - Champion Edition".
    bool DriverFullName(const char* gameName, std::string& outName);
    // Deterministic, peer-shared 128-bit identifier (32 hex chars) for a driver.
    // Used as a stand-in for the N64 ROM MD5 the lobby keys rooms and local rom
    // lookups on: arcade sets are identified by driver, so both peers agree, and
    // FBNeo itself verifies the actual rom data by CRC when loading.
    bool DriverRomId(const char* gameName, std::string& outHex);

    // Driver info.
    int PlayerCount();
    int InputBytesPerPlayer(); // 4 (one uint32 mask per player)

    // Per-player physical input masks. Index is 0-based.
    void SetLocalInput(int playerIndex, uint32_t mask);
    uint32_t GetLocalInput(int playerIndex);

    // Pause control for the frame loop. When paused, RunFrame keeps the last
    // frame on screen (and outputs silence) but doesn't step the driver.
    void SetPaused(bool paused);
    bool IsPaused();

    // DIP switches. The driver exposes a flat table of DIP entries (see
    // BurnDrvGetDIPInfo): an optional offset marker, group headers (0xFE/0xFD),
    // per-group settings (each possibly spanning several lines), and per-byte
    // defaults (0xFF). These helpers group the settings by header so a dialog
    // can list "Coinage: 1 Coin 1 Credit" style entries and select one.
    struct DipGroupInfo
    {
        std::string name;        // group header text (e.g. "Coinage")
        int         settingCount = 0;
        int         selectedSetting = -1; // index of the active setting, -1 if none
    };
    int  GetDipGroupCount();
    bool GetDipGroup(int group, DipGroupInfo& out);
    // Display name of a setting: "<group>: <setting>" (or just "<setting>" when
    // the group has no text).
    bool GetDipSettingName(int group, int setting, std::string& outName);
    // Select a setting within a group (RMW on the DIP byte) and remember it so
    // it survives driver resets.
    bool SetDipSelection(int group, int setting);
    // Apply the driver's per-byte defaults (0xFF entries) and forget overrides.
    void ResetDipsToDefaults();
    // Bulk override of DIP byte values (BurnInputInfo index -> value), applied
    // on top of the driver defaults. Stored so a driver reset keeps them.
    void SetDipOverrides(const std::vector<std::pair<int, uint8_t> >& overrides);
    void GetDipOverrides(std::vector<std::pair<int, uint8_t> >& out);

    // Ordered list of the driver's per-player digital input names -- the same
    // order the mask bits use (bit b corresponds to GetPlayerInputName(p, b)),
    // e.g. "P1 Coin", "P1 Start", "P1 Up", ... Frontends use these to build a
    // name-based key map so bindings survive per-driver input reordering.
    int GetPlayerInputCount(int playerIndex);
    const char* GetPlayerInputName(int playerIndex, int bit);

    // Enumerate a driver's per-player digital input names without starting a
    // session (no romset/BIOS required -- only the driver table). outPerPlayer[p]
    // holds player p's input names in the same mask-bit order GetPlayerInputName
    // uses. Returns false when the driver is unknown or a session is running.
    bool DriverInputNames(const char* gameName, std::vector<std::vector<std::string> >& outPerPlayer);

    // Round-trip a complete driver state through FBNeo's buffer serializer.
    bool SaveStateInto(unsigned char* buffer, int capacity, int& outLen, int& outChecksum, int frame);
    // Allocates and returns a heap-allocated state buffer (caller frees).
    bool SaveStateAlloc(unsigned char*& outBuffer, int& outLen, int& outChecksum, int frame);
    bool LoadState(const unsigned char* buffer, int len);

    // Run a single emulated frame (applies latched inputs).
    bool RunFrame(int flags);

    // Drive the rollback frame loop, mirroring M64CMD_ROLLBACK_EXECUTE:
    //   while begin_frame() == 1: run one visible frame; end_frame()
    bool Execute(const m64p_rollback_execute_callbacks& callbacks);

    // Sample the current physical input masks into values/players.
    bool SampleInput(void* values, int size, int players);

    // Input callback bookkeeping (set by the rollback layer).
    void SetInputCallback(int (*callback)(void* values, int size, int players));
    void SetInputPlayers(int players);

    // FBNeo has no equivalents for these; kept for API parity.
    void SetDeterministic(bool enabled);
    void SetTimesyncScale(double scale);
    void SetVerboseStats(bool enabled);

    // Last rendered frame / audio (valid after RunFrame(Output_Video/Output_Audio)).
    const VideoFrame& GetVideoFrame();
    const int16_t* GetSoundBuffer(int& sampleFrames);

    // Orientation flags reported to the presenter.
    enum VideoOrientationFlags
    {
        kVideoVertical = 1 << 0, // driver is rotated 90 degrees
        kVideoFlipped  = 1 << 1, // output is vertically flipped
    };

    // Per-visible-frame presentation callback. Invoked on the emulation thread
    // right after the visible frame has been stepped, so the GL context owned by
    // the frontend's render widget is current. audio is interleaved int16 stereo.
    typedef void (*FrameCallback)(
        const void* video, int width, int height, int pitch, int orientationFlags,
        const int16_t* audio, int audioFrames, void* user);

    void SetFrameCallback(FrameCallback callback, void* user);

    // Timing of the most recent frame (mupen fields RMG reads are populated,
    // the rest are zero).
    void GetRunFrameStats(m64p_rollback_run_frame_stats& out);
}

#endif // CORE_FBNEO_RUNTIME_HPP
