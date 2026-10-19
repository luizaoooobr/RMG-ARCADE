/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef CORE_EMULATION_HPP
#define CORE_EMULATION_HPP

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

enum class CoreEmulationState
{
    Stopped = 1,
    Running,
    Paused
};

// starts emulation with given ROM
bool CoreStartEmulation(std::filesystem::path n64rom, std::filesystem::path n64ddrom, std::string address = "", int port = -1, int player = -1);

// stops emulation
bool CoreStopEmulation(void);

// FBNeo presentation hooks. The frame callback is invoked on the emulation
// thread once per visible frame (video + audio), which is where the frontend's
// render widget owns a current OpenGL context. The stop callback runs once
// after the FBNeo frame loop ends so the frontend can tear its output down.
typedef void (*CoreFbneoFrameCallback)(
    const void* video, int width, int height, int pitch, int orientationFlags,
    const int16_t* audio, int audioFrames, void* user);
typedef void (*CoreFbneoStopCallback)(void* user);

void CoreSetFbneoFrameCallback(CoreFbneoFrameCallback callback, void* user);
void CoreSetFbneoStopCallback(CoreFbneoStopCallback callback, void* user);

// FBNeo live input. playerIndex is 0-based. The mask bit order matches the
// names returned by CoreGetFbneoPlayerInputName (bit b == input b). These are
// safe to call from any thread and are no-ops outside an FBNeo session.
void     CoreSetFbneoLocalInput(int playerIndex, uint32_t mask);
uint32_t CoreGetFbneoLocalInput(int playerIndex);
int      CoreGetFbneoPlayerInputCount(int playerIndex);
const char* CoreGetFbneoPlayerInputName(int playerIndex, int bit);

// Per-player digital input names for an arbitrary arcade driver, enumerated
// without starting a session (no romset/BIOS required). outPerPlayer[p] holds
// player p's input names in mask-bit order. Returns false for an unknown driver.
// The frontend uses this to configure arcade controls before playing.
bool CoreGetFbneoDriverInputNames(const std::string& gameName,
                                  std::vector<std::vector<std::string> >& outPerPlayer);

// Arcade (FBNeo) rom detection, used by the rom browser to list arcade sets
// alongside N64 roms. Returns true when `file` is a zip/7z archive whose stem
// matches an FBNeo driver, filling the driver's short name (e.g. "sf2ce"), its
// full display name, and a peer-shared 128-bit rom id (serves as the "MD5" the
// lobby uses to match rooms and locate the local rom).
bool CoreGetFbneoRomInfo(const std::filesystem::path& file, std::string& shortName, std::string& fullName, std::string& romId);

// short name (e.g. "sf2ce") of the driver backing the current arcade session,
// empty when no arcade session is running. Used by the frontend to key
// per-driver settings sections (input bindings, DIP switches, ...).
std::string CoreGetFbneoDriverName(void);

// FBNeo DIP switches. Groups/settings index into the driver's DIP table (the
// same list the native FBNeo DIP dialog shows). Selections are written into the
// live driver memory and remembered across resets; the frontend persists them
// per-driver. All are safe to call from any thread (no-ops outside a session)
// and read-only while a synchronized netplay session is active.
int  CoreGetFbneoDipGroupCount(void);
bool CoreGetFbneoDipGroup(int group, std::string& name, int& settingCount, int& selectedSetting);
bool CoreGetFbneoDipSettingName(int group, int setting, std::string& outName);
bool CoreSetFbneoDip(int group, int setting);
void CoreResetFbneoDips(void);
void CoreSetFbneoDipOverrides(const std::vector<std::pair<int, uint8_t> >& overrides);
void CoreGetFbneoDipOverrides(std::vector<std::pair<int, uint8_t> >& out);

// pauses emulation
bool CorePauseEmulation(void);

// resumes emulation
bool CoreResumeEmulation(void);

// advances emulation by one frame (must be paused)
bool CoreAdvanceFrame(void);

// advances emulation by several frames
bool CoreAdvanceFrames(int frames);

enum CoreFrameOutputFlags
{
    CoreFrameOutput_None   = 0,
    CoreFrameOutput_Video  = 1 << 0,
    CoreFrameOutput_Audio  = 1 << 1,
    CoreFrameOutput_Pacing = 1 << 2,
    CoreFrameOutput_Input  = 1 << 3,
    CoreFrameOutput_All    = CoreFrameOutput_Video | CoreFrameOutput_Audio | CoreFrameOutput_Pacing | CoreFrameOutput_Input
};

// configures frame side effects used by normal, hidden, and rollback frames
bool CoreSetFrameOutput(int flags);

// advances emulation by several frames using per-call frame side effects
bool CoreRunFrames(int frames, int flags);

// resets emulation
bool CoreResetEmulation(bool hard);

// returns whether emulation is running
bool CoreIsEmulationRunning(void);

// returns whether emulation is paused
bool CoreIsEmulationPaused(void);

// returns whether synchronized netplay is active
bool CoreIsSynchronizedNetplayActive(void);

// returns whether the current emulation session is an arcade (FBNeo) session,
// which is driven by the rollback frame loop rather than the mupen64plus core
bool CoreIsFbneoSessionActive(void);

// returns current VI (vertical interrupt) frame count
// used for synchronization in netplay/Kaillera
int CoreGetCurrentFrameCount(void);

// Spectate keyframe restore. Stage a full in-memory savestate for the
// spectator's emulation to load on its emulation thread before it consumes the first
// recorded input. frame identifies the first krec record after the saved state.
// Stage BEFORE the spectate emulation starts. Clear when not spectating.
void CoreStageSpectateKeyframe(const unsigned char* data, int len, int frame);
void CoreClearSpectateKeyframe(void);

#endif // CORE_EMULATION_HPP
