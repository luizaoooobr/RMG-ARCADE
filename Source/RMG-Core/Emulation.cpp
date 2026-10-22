/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "MediaLoader.hpp"
#include "RomSettings.hpp"
#include "Emulation.hpp"
#include "RomHeader.hpp"
#include "Settings.hpp"
#include "SaveState.hpp"
#include "Library.hpp"
#include "Netplay.hpp"
#include "Kaillera.hpp"
#include "Plugins.hpp"
#include "Cheats.hpp"
#include "Error.hpp"
#include "Callback.hpp"
#include "File.hpp"
#include "Rom.hpp"
#include "rmgk_gekko.hpp"
#include "FbneoRuntime.hpp"
#ifdef RMGK_HAVE_P2P_TRANSPORT
#include "n02_client.h"
#endif

#include "m64p/Api.hpp"

#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <vector>

// Windows/POSIX dynamic loading
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static void setRollbackLoggingEnvironment(void)
{
    const bool pifLogging = CoreSettingsGetBoolValue(SettingsID::Rollback_VerbosePifInputLogging);
    const bool glideLogging = CoreSettingsGetBoolValue(SettingsID::Rollback_VerboseGlideInputLogging);
#ifdef _WIN32
    _putenv_s("RMGK_VERBOSE_PIF_INPUT_LOGGING", pifLogging ? "1" : "0");
    _putenv_s("RMGK_VERBOSE_GLIDE_INPUT_LOGGING", glideLogging ? "1" : "0");
#else
    setenv("RMGK_VERBOSE_PIF_INPUT_LOGGING", pifLogging ? "1" : "0", 1);
    setenv("RMGK_VERBOSE_GLIDE_INPUT_LOGGING", glideLogging ? "1" : "0", 1);
#endif
}

// Forward declarations for PIF structures
extern "C" {
    struct pif;
    struct pif_channel {
        void* jbd;
        const void* ijbd;
        uint8_t* tx;
        uint8_t* tx_buf;
        uint8_t* rx;
        uint8_t* rx_buf;
    };
    enum {
        PIF_CHANNELS_COUNT = 5,
        PIF_CONTROLLER_CHANNELS_COUNT = 4
    };

    struct pif {
        uint8_t* base;
        uint8_t* ram;
        struct pif_channel channels[PIF_CHANNELS_COUNT];
    };

    // Joybus command constants
    enum {
        JCMD_STATUS = 0x00,
        JCMD_CONTROLLER_READ = 0x01,
        JCMD_PAK_READ = 0x02,
        JCMD_PAK_WRITE = 0x03,
        JCMD_EEPROM_READ = 0x04,
        JCMD_EEPROM_WRITE = 0x05,
        JCMD_RESET = 0xff
    };

    typedef void (*pif_sync_callback_t)(struct pif*);
}

static bool parse_gekko_address(const std::string& address, std::string& remoteAddress, int& remotePort, int& frameDelay, int& predictionWindow)
{
    // GEKKO| (P2P / n02-transport) carries a single remote peer.
    // Format: GEKKO|remoteIp|remotePort|delay[|prediction]
    constexpr const char* prefix = "GEKKO|";
    if (address.rfind(prefix, 0) != 0)
    {
        return false;
    }

    const size_t remoteStart = std::char_traits<char>::length(prefix);
    const size_t portSeparator = address.find('|', remoteStart);
    if (portSeparator == std::string::npos)
    {
        return false;
    }
    const size_t delaySeparator = address.find('|', portSeparator + 1);
    if (delaySeparator == std::string::npos)
    {
        return false;
    }
    const size_t predictionSeparator = address.find('|', delaySeparator + 1);

    remoteAddress = address.substr(remoteStart, portSeparator - remoteStart);
    try
    {
        remotePort = std::stoi(address.substr(portSeparator + 1, delaySeparator - portSeparator - 1));
        if (predictionSeparator == std::string::npos)
        {
            frameDelay = std::stoi(address.substr(delaySeparator + 1));
            predictionWindow = 7;
        }
        else
        {
            frameDelay = std::stoi(address.substr(delaySeparator + 1, predictionSeparator - delaySeparator - 1));
            predictionWindow = std::stoi(address.substr(predictionSeparator + 1));
        }
    }
    catch (...)
    {
        return false;
    }

    return !remoteAddress.empty() && remotePort > 0 && remotePort <= 65535 && frameDelay >= 0 && predictionWindow >= 1;
}

// LOBBY| carries N-1 remote peers, each with its own slot and endpoint, so
// 3-/4-player rollback sessions can wire each remote actor to a distinct peer.
// Format: LOBBY|<delay>|<prediction>|<slot>,<ip>,<port>|<slot>,<ip>,<port>...
static bool parse_lobby_address(const std::string& address, int& frameDelay, int& predictionWindow,
    std::vector<LobbyRemotePeer>& remotes)
{
    remotes.clear();

    constexpr const char* prefix = "LOBBY|";
    if (address.rfind(prefix, 0) != 0)
    {
        return false;
    }

    // Tokenize on '|' starting after the prefix.
    std::vector<std::string> tokens;
    size_t pos = std::char_traits<char>::length(prefix);
    while (pos <= address.size())
    {
        const size_t next = address.find('|', pos);
        if (next == std::string::npos)
        {
            tokens.push_back(address.substr(pos));
            break;
        }
        tokens.push_back(address.substr(pos, next - pos));
        pos = next + 1;
    }

    // Need at least: delay, prediction, and one peer entry.
    if (tokens.size() < 3)
    {
        return false;
    }

    try
    {
        frameDelay = std::stoi(tokens[0]);
        predictionWindow = std::stoi(tokens[1]);
    }
    catch (...)
    {
        return false;
    }
    if (frameDelay < 0 || predictionWindow < 1)
    {
        return false;
    }

    for (size_t i = 2; i < tokens.size(); i++)
    {
        const std::string& tok = tokens[i];
        const size_t firstComma = tok.find(',');
        if (firstComma == std::string::npos)
        {
            return false;
        }
        const size_t secondComma = tok.find(',', firstComma + 1);
        if (secondComma == std::string::npos)
        {
            return false;
        }
        LobbyRemotePeer peer{};
        try
        {
            peer.slot = std::stoi(tok.substr(0, firstComma));
            peer.ip = tok.substr(firstComma + 1, secondComma - firstComma - 1);
            if (peer.ip == "ice")
            {
                peer.userId = std::stoull(tok.substr(secondComma + 1));
                if (peer.userId == 0)
                    return false;
            }
            else
            {
                const int parsedPort = std::stoi(tok.substr(secondComma + 1));
                if (parsedPort <= 0 || parsedPort > 65535 || peer.ip.empty())
                    return false;
                peer.port = static_cast<unsigned short>(parsedPort);
            }
            if (peer.slot < 1 || peer.slot > 4)
                return false;
        }
        catch (...)
        {
            return false;
        }
        remotes.push_back(std::move(peer));
    }

    return !remotes.empty();
}

//
// Local Variables
//

// Frame counter for Kaillera sync (updated via frame callback)
static int s_CurrentFrame = 0;
static bool s_FbneoActive = false;
// True while the current emulation session is an Arcade (FBNeo) session. Stays
// set until a new (non-FBNeo) session starts, so CoreStopEmulation can reliably
// avoid the mupen64plus M64CMD_STOP path even after the FBNeo runtime is down.
static bool s_FbneoSessionActive = false;

#ifdef NETPLAY
// Maximum players supported by Kaillera
#define MAX_PLAYERS 8

// Cache for preventing duplicate syncs within same frame
static int s_LastSyncFrame = -1;
static uint32_t s_CachedSyncBuffer[MAX_PLAYERS] = {0};
static int s_CachedNumReceived = 0;

// Track whether we've already synced since the last frame advance
// This is more reliable than comparing frame numbers due to callback timing
static bool s_SyncedThisFrame = false;

static bool pif_channel_has_command(const pif_channel& channel)
{
    return channel.tx != nullptr &&
           channel.rx != nullptr &&
           channel.tx_buf != nullptr &&
           channel.rx_buf != nullptr;
}
#endif
// Frame callback function
static void FrameCallback(unsigned int frameIndex)
{
    s_CurrentFrame = frameIndex;
#ifdef NETPLAY
    // Reset sync flag at the start of each new frame
    // This ensures we sync exactly once per frame regardless of PIF polling timing
    s_SyncedThisFrame = false;
#endif
}

// Spectate keyframe restore: a savestate the spectator must load before consuming any
// recorded input, so its replay starts from the broadcaster's snapshot (frame F) rather
// than boot. Staged from the UI thread (CoreStageSpectateKeyframe); loaded exactly once
// on the emulation thread in the PIF sync below â€” the rollback load is synchronous, so it
// must run on the emulation thread, not cross-thread from the UI.
static std::mutex          s_SpectateKeyframeMutex;
static std::vector<unsigned char> s_SpectateKeyframeBuf;
static int                 s_SpectateKeyframeFrame = -1;
static bool                s_SpectateKeyframeStaged = false;
static bool                s_SpectateKeyframeLoaded = false;

// Spectate keyframe divergence probe (diagnostic): after loading a keyframe, hash the
// just-restored state (= the broadcaster's S(frame)) and the state at the start of each
// of the next frames the spectator replays, dumping them to <prefix>_spectate.log so
// they can be diffed against the broadcaster's per-frame host probe to find the first
// divergent frame. All probe state below is touched only on the emulation thread.
// -1 = inactive; 1..N = how many replayed frames still to hash.
static constexpr bool      kSpectateProbeEnabled = false;
static int                 s_SpectateProbeSeq = -1;
static int                 s_SpectateProbeFrameBase = -1;
static std::vector<unsigned char> s_SpectateProbeScratch;
// Diagnostic: hash of the spectator's state at the moment the deferred keyframe load was
// staged (pre-jump). Compared at seq 0 to tell "load never drained" (resaved==prejump)
// from "load landed" (resaved==kfbuf).
static uint64_t            s_SpectatePreJumpHash = 0;
// Companion to the state probe: logs the actual controller input the spectator APPLIES
// each frame after the keyframe load, so it can be diffed against the broadcaster's
// recorded krec input per frame. Tells us whether the boundary divergence is an input
// misalignment (inputs differ / are shifted) or hidden state (inputs identical but the
// state still forks). -1 = inactive; 0..N = frames applied since load.
static int                 s_SpectateInputProbeCount = -1;

// Kaillera PIF sync callback (called from mupen64plus-core after netplay sync)
static void KailleraPifSyncCallback(struct pif* pif)
{
#ifdef NETPLAY
    if (rmgk_gekko::is_netplay_session_active()) {
        return;
    }

    if (!CoreHasInitKaillera()) {
        return;
    }

    int player_num = CoreGetKailleraPlayerNumber();
    int num_players = CoreGetKailleraNumPlayers();

    if (player_num < 1 || player_num > MAX_PLAYERS) {
        return; // Invalid player number
    }

    // Playback/spectate has no local player: every player's input comes from the krec.
    // In this mode we must NEVER read or inject this machine's physical controller â€”
    // doing so was overriding the recorded input with the viewer's idle joystick and
    // desyncing the replay. Live netplay (false) still submits the local controller.
    const bool inPlayback = CoreIsKailleraPlaybackMode();

    // Check if this is a controller read command for channel 0 (local player)
    // We only want to sync on actual input reads, not status queries or other commands
    bool isControllerRead = (pif_channel_has_command(pif->channels[0]) &&
                             pif->channels[0].tx_buf[0] == JCMD_CONTROLLER_READ);

    // Only sync with Kaillera on controller read commands, and only once per frame
    // This prevents syncing on JCMD_STATUS which would send zero input
    if (isControllerRead && !s_SyncedThisFrame) {
        // First controller read this frame - read local input and sync with Kaillera
        s_SyncedThisFrame = true;  // Mark as synced BEFORE calling Kaillera

        // Spectate keyframe restore: the first controller read after a keyframe is
        // staged, jump to the broadcaster's snapshot before consuming any
        // recorded input. The load must NOT happen synchronously here â€” this runs inside
        // an SI DMA (dma_si_read -> update_pif_ram -> sync callback), and a full machine
        // load mid-SI / mid-dynarec-block leaves the half-finished SI completion and the
        // rest of the current block running on top of the restored state, desyncing at
        // F+1. Instead defer it: the core performs the load at its next safe interrupt
        // boundary (gen_interrupt), exactly like a normal savestate load, then the game
        // re-dispatches cleanly from the restored PC and consumes the first tail
        // record. The broadcaster captured this state after frame F, so that record is
        // F+1â€”not F. (One controller poll per frame is assumed: gen_interrupt fires between
        // this pre-load read and the next read, so the load is done by then.)
        {
            std::lock_guard<std::mutex> kfLock(s_SpectateKeyframeMutex);
            // Wait until the krec tail has actually arrived (>=1 indexed input record)
            // before loading. The server slices the tail to the first input after the
            // saved state, so frameIndex[0] is exactly that next record. Loading before
            // it arrives would leave nothing to realign the reader to.
#ifdef RMGK_HAVE_P2P_TRANSPORT
            if (s_SpectateKeyframeStaged && !s_SpectateKeyframeLoaded && !s_SpectateKeyframeBuf.empty() &&
                n02::playbackGetTotalFrames() > 0) {
                CoreRollbackState kfState{};
                kfState.buffer = s_SpectateKeyframeBuf.data();
                kfState.len    = static_cast<int>(s_SpectateKeyframeBuf.size());
                kfState.frame  = s_SpectateKeyframeFrame;
                const bool deferOk = CoreRollbackLoadGameStateDeferred(kfState);

                // Realign the krec reader to the first record after the snapshot. ROM boot
                // may poll controllers before the deferred load lands; seeking to zero
                // ensures none of those speculative reads shift the post-load input tail.
                const bool seekOk = n02::playbackSeekToFrame(0);
                if (!deferOk || !seekOk)
                {
                    CoreSetError("Could not align the live replay keyframe with its input tail");
                    n02::playbackStopStream();
                    return;
                }
                s_SpectateKeyframeLoaded = true;

                if (kSpectateProbeEnabled)
                {
                    // Optional diagnostic: compare post-load state/input hashes with the
                    // broadcaster when investigating a deterministic divergence.
                    s_SpectateProbeFrameBase = s_SpectateKeyframeFrame;
                    s_SpectateProbeScratch.assign(s_SpectateKeyframeBuf.size() + 65536, 0);
                    s_SpectateProbeSeq = 0;
                    s_SpectatePreJumpHash = 0;
                    CoreRollbackState pj{};
                    if (CoreRollbackSaveGameStateInto(pj, s_SpectateProbeScratch.data(),
                                                      static_cast<int>(s_SpectateProbeScratch.size()),
                                                      s_SpectateKeyframeFrame)) {
                        s_SpectatePreJumpHash = rmgk_gekko::hash_bytes(pj.buffer,
                                                    static_cast<size_t>(pj.len));
                    }
                    std::ostringstream sc;
                    sc << "spectate_stage frame=" << s_SpectateKeyframeFrame
                       << " kflen=" << s_SpectateKeyframeBuf.size()
                       << " prejump_hash=" << std::hex << s_SpectatePreJumpHash << std::dec
                       << " tail_frames=" << n02::playbackGetTotalFrames()
                       << " defer_ok=" << (deferOk ? 1 : 0)
                       << " seek_ok=" << (seekOk ? 1 : 0);
                    rmgk_gekko::write_spectate_probe(sc.str());
                    s_SpectateInputProbeCount = 0;
                }
                else
                {
                    s_SpectateProbeSeq = -1;
                    s_SpectateProbeFrameBase = -1;
                    s_SpectateProbeScratch.clear();
                    s_SpectatePreJumpHash = 0;
                    s_SpectateInputProbeCount = -1;
                }

                // Don't consume a krec record on this pre-load read; clear the per-frame
                // sync flag so the first read after the deferred load consumes tail[0].
                s_SyncedThisFrame = false;
                return;
            }
#endif
        }

        // Divergence probe seq 1..N: each subsequent frame's first controller read runs
        // at the start of that frame, i.e. AFTER the previous frame fully advanced â€” so
        // re-saving here fingerprints the spectator's state at the start of replayed
        // frame (base + seq), comparable to the broadcaster's host probe for that frame.
        // A mismatch starting at base+1 means the very first replayed input is wrong
        // (boundary alignment); a mismatch only after many clean frames means a slow
        // reseed from state the savestate doesn't carry.
        if (s_SpectateProbeSeq >= 0 && s_SpectateProbeSeq <= 30) {
            const int probeFrame = s_SpectateProbeFrameBase + s_SpectateProbeSeq;
            CoreRollbackState probe{};
            if (CoreRollbackSaveGameStateInto(probe, s_SpectateProbeScratch.data(),
                                              static_cast<int>(s_SpectateProbeScratch.size()), probeFrame)) {
                const uint64_t h = rmgk_gekko::hash_bytes(probe.buffer, static_cast<size_t>(probe.len));
                std::ostringstream stream;
                stream << "spectate_probe side=spec frame=" << probeFrame
                       << " hash=" << std::hex << h << std::dec
                       << " len=" << probe.len;
                rmgk_gekko::write_spectate_probe(stream.str());

                // DECISIVE load-landed check: seq 0 is the first read after the deferred
                // keyframe load should have drained at gen_interrupt. If the load landed,
                // re-saving here reproduces the keyframe buffer byte-for-byte (Phase 1
                // idempotency), so resaved_hash == kfbuf_hash. If they differ, the load
                // never executed (the spectator is still on its own boot/replay timeline).
                if (s_SpectateProbeSeq == 0) {
                    uint64_t kfHash = 0; size_t kfLen = 0;
                    {
                        std::lock_guard<std::mutex> kfLock(s_SpectateKeyframeMutex);
                        kfLen = s_SpectateKeyframeBuf.size();
                        if (kfLen > 0)
                            kfHash = rmgk_gekko::hash_bytes(s_SpectateKeyframeBuf.data(), kfLen);
                    }
                    const char* verdict = (kfLen > 0 && kfHash == h) ? "landed"
                                        : (h == s_SpectatePreJumpHash)  ? "NOT_DRAINED"
                                        : "other";
                    std::ostringstream lc;
                    lc << "spectate_loadcheck frame=" << probeFrame
                       << " kfbuf_hash=" << std::hex << kfHash
                       << " resaved_hash=" << h
                       << " prejump_hash=" << s_SpectatePreJumpHash << std::dec
                       << " kflen=" << kfLen << " resavedlen=" << probe.len
                       << " verdict=" << verdict;
                    rmgk_gekko::write_spectate_probe(lc.str());
                }
            }
            s_SpectateProbeSeq++;
        }

        // Live netplay submits THIS client's local controller in slot 0 and gets back
        // all players' synced inputs. In playback/spectate slot 0 must stay zero so the
        // krec fully populates the buffer â€” reading the local controller here is what
        // drove the spectator's P1 with the viewer's joystick.
        uint32_t sync_buffer[MAX_PLAYERS] = {0};
        if (!inPlayback) {
            uint8_t* rx = pif->channels[0].rx_buf;
            sync_buffer[0] = (rx[0] << 24) | (rx[1] << 16) | (rx[2] << 8) | rx[3];
        }

        // Synchronize with Kaillera - this must be called exactly ONCE per emulator frame
        int ret = CoreModifyKailleraPlayValues(sync_buffer, sizeof(uint32_t));

        if (ret < 0) {
            // Game ended or network error. For live netplay we keep emulation
            // running so the user can manually exit; for krec playback there
            // are no more inputs to feed, so stop emulation here (the dialog
            // timer would otherwise only catch this while the dialog is open).
            const bool wasPlayback = CoreIsKailleraPlaybackMode();
            CoreMarkKailleraGameInactive();
            s_CachedNumReceived = 0;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                s_CachedSyncBuffer[i] = 0;
            }
            if (wasPlayback) {
                CoreStopEmulation();
            }
            return;
        }

        if (ret == 0) {
            // No fresh synced input this frame. Live netplay: frame-delay buffering â€”
            // return and let the previous cached input stand. Playback/spectate: the
            // next krec record hasn't arrived yet (live edge); do NOT return, because the
            // viewer's physical controller is still sitting in PIF RAM and would drive
            // this frame. Fall through so the PIF write re-applies the last cached krec
            // input (or forced neutral) instead of the local controller.
            if (!inPlayback) {
                return;
            }
        } else {
            int num_received = ret / sizeof(uint32_t);

            // Cache synced results for subsequent polls this frame and for writing to PIF
            s_CachedNumReceived = num_received;
            for (int i = 0; i < MAX_PLAYERS; i++) {
                s_CachedSyncBuffer[i] = sync_buffer[i];
            }

            // Spectate input probe: log the input the spectator actually applies this
            // frame so it lines up with the broadcaster's recorded krec bytes.
            if (s_SpectateInputProbeCount >= 0 && s_SpectateInputProbeCount <= 30 && num_received > 0) {
                std::ostringstream istream;
                istream << "spectate_input side=spec frame=" << (s_SpectateProbeFrameBase + s_SpectateInputProbeCount)
#ifdef RMGK_HAVE_P2P_TRANSPORT
                        << " mode=" << n02::getActiveMode()
#endif
                        << " ipb=" << (inPlayback ? 1 : 0);
                for (int p = 0; p < num_received && p < 2; p++) {
                    char buf[16];
                    snprintf(buf, sizeof(buf), " p%d=%08x", p, s_CachedSyncBuffer[p]);
                    istream << buf;
                }
                rmgk_gekko::write_spectate_probe(istream.str());
                s_SpectateInputProbeCount++;
            }
        }
    }

    // Write synchronized inputs to PIF RAM for all netplay players.
    // JCMD_STATUS/JCMD_RESET are handled unconditionally so games detect
    // controllers even before the first Kaillera sync (needed for playback
    // without a physical controller connected).
    // JCMD_CONTROLLER_READ data injection is gated on cache availability.
    int numPlayers = CoreGetKailleraNumPlayers();
    if (numPlayers > PIF_CONTROLLER_CHANNELS_COUNT) {
        numPlayers = PIF_CONTROLLER_CHANNELS_COUNT;
    }

    for (int i = 0; i < numPlayers && i < MAX_PLAYERS; i++) {
        if (pif_channel_has_command(pif->channels[i])) {
            // Always clear error bits to show controller as connected
            *pif->channels[i].rx &= ~0xC0;

            uint8_t cmd = pif->channels[i].tx_buf[0];

            if (cmd == JCMD_STATUS || cmd == JCMD_RESET) {
                // Controller detection - force standard controller type response
                if (pif->channels[i].rx_buf != NULL) {
                    uint16_t type = 0x0500; // JDT_JOY_ABS_COUNTERS | JDT_JOY_PORT
                    pif->channels[i].rx_buf[0] = (uint8_t)(type >> 0);
                    pif->channels[i].rx_buf[1] = (uint8_t)(type >> 8);
                    pif->channels[i].rx_buf[2] = 0; // No pak status
                }
            }
            else if (cmd == JCMD_CONTROLLER_READ) {
                // Write synced controller input from cache (only when populated)
                if (s_CachedNumReceived > 0 && i < s_CachedNumReceived && pif->channels[i].rx_buf != NULL) {
                    uint8_t* rx = pif->channels[i].rx_buf;
                    rx[0] = (s_CachedSyncBuffer[i] >> 24) & 0xFF;
                    rx[1] = (s_CachedSyncBuffer[i] >> 16) & 0xFF;
                    rx[2] = (s_CachedSyncBuffer[i] >> 8) & 0xFF;
                    rx[3] = s_CachedSyncBuffer[i] & 0xFF;
                }
                else if (inPlayback && pif->channels[i].rx_buf != NULL) {
                    // Playback with no synced krec input yet: force neutral so the
                    // viewer's local controller (already written here by the input
                    // plugin) can never drive the replay.
                    uint8_t* rx = pif->channels[i].rx_buf;
                    rx[0] = rx[1] = rx[2] = rx[3] = 0;
                }
            }
            else if (cmd == JCMD_PAK_READ && pif->channels[i].rx_buf != NULL) {
                // No controller pak present
                pif->channels[i].rx_buf[32] = 255;
            }
            else if (cmd == JCMD_PAK_WRITE && pif->channels[i].rx_buf != NULL) {
                // No controller pak present
                pif->channels[i].rx_buf[0] = 255;
            }
        }
    }
#endif // NETPLAY
}

void CoreStageSpectateKeyframe(const unsigned char* data, int len, int frame)
{
    // UI thread: stage a savestate for the spectator's emulation to restore on its
    // emulation thread before the first recorded input is consumed (see the PIF sync
    // callback). Stage BEFORE the spectate emulation starts so the load wins the race.
    std::lock_guard<std::mutex> lock(s_SpectateKeyframeMutex);
    if (data != nullptr && len > 0)
        s_SpectateKeyframeBuf.assign(data, data + len);
    else
        s_SpectateKeyframeBuf.clear();
    s_SpectateKeyframeFrame  = frame;
    s_SpectateKeyframeStaged = (data != nullptr && len > 0);
    s_SpectateKeyframeLoaded = false;
}

void CoreClearSpectateKeyframe(void)
{
    std::lock_guard<std::mutex> lock(s_SpectateKeyframeMutex);
    s_SpectateKeyframeBuf.clear();
    s_SpectateKeyframeFrame  = -1;
    s_SpectateKeyframeStaged = false;
    s_SpectateKeyframeLoaded = false;
    // Disarm the divergence probe so a partial window can't fire into a later session.
    s_SpectateProbeSeq        = -1;
    s_SpectateProbeFrameBase  = -1;
    s_SpectateInputProbeCount = -1;
}

//
// Local Functions
//

static bool get_emulation_state(m64p_emu_state& state)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY, M64CORE_EMU_STATE, &state);
    if (ret != M64ERR_SUCCESS)
    {
        error = "get_emulation_state m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

static void apply_coresettings_overlay(void)
{
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_RandomizeInterrupt));
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CPU_Emulator));
    CoreSettingsSetValue(SettingsID::Core_DisableExtraMem, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_DisableExtraMem));
    CoreSettingsSetValue(SettingsID::Core_EnableDebugger, CoreSettingsGetBoolValue(SettingsID::CoreOverlay_EnableDebugger));
    CoreSettingsSetValue(SettingsID::Core_CountPerOp, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CountPerOp));
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, CoreSettingsGetIntValue(SettingsID::CoreOverlay_CountPerOpDenomPot));
    CoreSettingsSetValue(SettingsID::Core_SiDmaDuration, CoreSettingsGetIntValue(SettingsID::CoreOverlay_SiDmaDuration));
    CoreSettingsSetValue(SettingsID::Core_SaveFileNameFormat, CoreSettingsGetIntValue(SettingsID::CoreOverLay_SaveFileNameFormat));
    CoreSettingsSetValue(SettingsID::Core_GbCameraVideoCaptureBackend1, CoreSettingsGetStringValue(SettingsID::CoreOverlay_GbCameraVideoCaptureBackend1));
    // Reset DisableSaveFileLoading to default (false) - Kaillera will override this later if needed
    CoreSettingsSetValue(SettingsID::Core_DisableSaveFileLoading, false);
}

static void apply_game_coresettings_overlay(void)
{
    std::string section;
    CoreRomSettings romSettings;
    bool overrideCoreSettings;

    // when we fail to retrieve the rom settings, return
    if (!CoreGetCurrentDefaultRomSettings(romSettings))
    {
        return;
    }

    section = romSettings.MD5;

    // when we don't need to override the core settings, return
    overrideCoreSettings = CoreSettingsGetBoolValue(SettingsID::Game_OverrideCoreSettings, section);
    if (!overrideCoreSettings)
    {
        return;
    }

    // apply settings overlay
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, CoreSettingsGetBoolValue(SettingsID::Game_RandomizeInterrupt, section));
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, CoreSettingsGetIntValue(SettingsID::Game_CPU_Emulator, section));
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, CoreSettingsGetIntValue(SettingsID::Game_CountPerOpDenomPot, section));
}

#ifdef NETPLAY
// Force HLE RSP plugin for Kaillera netplay - must be called BEFORE ROM open
// so the setting takes effect when plugins are loaded
static void apply_kaillera_rsp_override(void)
{
#ifdef _WIN32
    CoreSettingsSetValue(SettingsID::Core_RSP_Plugin, std::string("mupen64plus-rsp-hle.dll"));
#else
    CoreSettingsSetValue(SettingsID::Core_RSP_Plugin, std::string("mupen64plus-rsp-hle.so"));
#endif
}

// Force deterministic settings for Kaillera netplay to prevent desync
// These settings MUST be identical across all clients
// Called AFTER overlays so user/game settings don't override these
static void apply_kaillera_deterministic_settings(void)
{
    // Disable RandomizeInterrupt - critical for deterministic emulation
    // When enabled, interrupt timing varies randomly which causes desync
    CoreSettingsSetValue(SettingsID::Core_RandomizeInterrupt, false);

    // Use dynamic recompiler for best performance
    // Value 0 = Pure Interpreter, 1 = Cached Interpreter, 2 = Dynamic Recompiler
    CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, 2);

    // Set consistent CountPerOp values for deterministic timing
    CoreSettingsSetValue(SettingsID::Core_CountPerOp, 0);
    CoreSettingsSetValue(SettingsID::Core_CountPerOpDenomPot, 0);

    // Set consistent SI DMA duration
    CoreSettingsSetValue(SettingsID::Core_SiDmaDuration, -1);

    // Force extra memory enabled (8MB expansion pak) for consistent memory layout
    // Different memory configurations between players causes desync
    CoreSettingsSetValue(SettingsID::Core_DisableExtraMem, false);

    // Disable save file loading so all players start with fresh/empty saves
    // This prevents desync from players having different in-game settings saved
    CoreSettingsSetValue(SettingsID::Core_DisableSaveFileLoading, true);
}
#endif

static void apply_pif_rom_settings(void)
{
    CoreRomHeader romHeader;
    std::string error;
    m64p_error ret;
    int cpuEmulator;
    bool usePifROM;

    // when we fail to retrieve the rom settings, return
    if (!CoreGetCurrentRomHeader(romHeader))
    {
        return;
    }

    // when we're using the dynarec, return
    cpuEmulator = CoreSettingsGetIntValue(SettingsID::Core_CPU_Emulator);
    if (cpuEmulator >= 2)
    {
        return;
    }

    usePifROM = CoreSettingsGetBoolValue(SettingsID::Core_PIF_Use);
    if (!usePifROM)
    {
        return;
    }

    const SettingsID settingsIds[] =
    {
        SettingsID::Core_PIF_NTSC,
        SettingsID::Core_PIF_PAL,
    };

    std::string rom = CoreSettingsGetStringValue(settingsIds[static_cast<int>(romHeader.SystemType)]);
    if (!std::filesystem::is_regular_file(rom))
    {
        return;
    }

    std::vector<char> buffer;
    if (!CoreReadFile(rom, buffer))
    {
        return;
    }

    ret = m64p::Core.DoCommand(M64CMD_PIF_OPEN, buffer.size(), buffer.data());
    if (ret != M64ERR_SUCCESS)
    {
        error = "open_pif_rom m64p::Core.DoCommand(M64CMD_PIF_OPEN) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }
}

//
// Exported Functions
//

// FBNeo identifies a game by its driver name, which is the ROM archive's base
// name (sf2ce.zip -> "sf2ce").
static std::string fbneo_game_name(const std::filesystem::path& rom)
{
    return rom.stem().string();
}

// The archive list handed to FBNeo must hold the game's own archive plus any
// BIOS/parent archive FBNeo needs to resolve ROMs. NeoGeo, CPS-3 and IGS/PGM
// titles read their BIOS from a separate archive that ships beside the game, so
// we probe the ROM's directory for the known BIOS archives.
static std::vector<std::string> fbneo_rom_archives(const std::filesystem::path& rom)
{
    std::vector<std::string> archives;

    std::error_code ec;
    if (std::filesystem::is_regular_file(rom, ec))
    {
        archives.push_back(rom.string());
    }

    static const char* const kBiosArchives[] = {
        "neogeo.zip", // NeoGeo MVS/AES BIOS
        "pgm.zip",    // IGS PGM BIOS
        "skns.zip",   // Super Kaneko Nova System BIOS
    };

    const std::filesystem::path dir = rom.parent_path();
    for (const char* bios : kBiosArchives)
    {
        const std::filesystem::path candidate = dir / bios;
        if (std::filesystem::is_regular_file(candidate, ec))
        {
            archives.push_back(candidate.string());
        }
    }

    return archives;
}

static std::vector<const char*> fbneo_archive_ptrs(const std::vector<std::string>& store)
{
    std::vector<const char*> ptrs;
    ptrs.reserve(store.size());
    for (const auto& entry : store)
    {
        ptrs.push_back(entry.c_str());
    }
    return ptrs;
}

// True when the path is an archive whose base name matches an FBNeo driver
// (sf2ce.zip -> "sf2ce"). Arcade romsets must be routed to the FBNeo backend
// instead of the mupen64plus N64 ROM path, which rejects them at open time.
static bool fbneo_is_driver_archive(const std::filesystem::path& rom)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(rom, ec))
        return false;

    std::string ext = rom.extension().string();
    for (char& c : ext)
    {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    if (ext != ".zip" && ext != ".7z")
        return false;

    return fbneo_rt::IsDriver(rom.stem().string().c_str());
}

static std::string fbneo_dip_section(const std::string& driverName)
{
    return "RMG Arcade DIPs - " + driverName;
}

// Re-apply the persisted DIP selections for `driverName` on top of the driver
// defaults fbneo_rt::Initialize already seeded. Called once per local (non-netplay)
// session start; netplay sessions skip this so both peers run the identical
// driver defaults and stay bit-identical.
static void fbneo_load_dip_settings(const std::string& driverName)
{
    if (driverName.empty()) return;
    std::string section = fbneo_dip_section(driverName);
    std::vector<int> groups   = CoreSettingsGetIntListValue(SettingsID::FBNeo_DipGroups, section);
    std::vector<int> settings = CoreSettingsGetIntListValue(SettingsID::FBNeo_DipSettings, section);
    size_t n = groups.size() < settings.size() ? groups.size() : settings.size();
    for (size_t i = 0; i < n; i++)
        fbneo_rt::SetDipSelection(groups[i], settings[i]);
}

// Persist one (group, setting) choice for `driverName`, replacing any previous
// choice for the same group. Parallel int lists under "RMG Arcade DIPs - <driver>".
static void fbneo_persist_dip_selection(const std::string& driverName, int group, int setting)
{
    if (driverName.empty()) return;
    std::string section = fbneo_dip_section(driverName);
    std::vector<int> groups   = CoreSettingsGetIntListValue(SettingsID::FBNeo_DipGroups, section);
    std::vector<int> settings = CoreSettingsGetIntListValue(SettingsID::FBNeo_DipSettings, section);
    std::vector<int> newGroups, newSettings;
    for (size_t i = 0; i < groups.size() && i < settings.size(); i++)
    {
        if (groups[i] == group) continue;
        newGroups.push_back(groups[i]);
        newSettings.push_back(settings[i]);
    }
    newGroups.push_back(group);
    newSettings.push_back(setting);
    CoreSettingsSetValue(SettingsID::FBNeo_DipGroups, section, newGroups);
    CoreSettingsSetValue(SettingsID::FBNeo_DipSettings, section, newSettings);
}

// Forget any persisted DIP choices for `driverName`.
static void fbneo_clear_dip_settings(const std::string& driverName)
{
    if (driverName.empty()) return;
    std::string section = fbneo_dip_section(driverName);
    CoreSettingsSetValue(SettingsID::FBNeo_DipGroups, section, std::vector<int>());
    CoreSettingsSetValue(SettingsID::FBNeo_DipSettings, section, std::vector<int>());
}

// FBNeo presentation hooks registered by the frontend.
static CoreFbneoStopCallback s_fbneoStopCallback = nullptr;
static void*                s_fbneoStopUser     = nullptr;

void CoreSetFbneoFrameCallback(CoreFbneoFrameCallback callback, void* user)
{
    fbneo_rt::SetFrameCallback(
        reinterpret_cast<fbneo_rt::FrameCallback>(callback), user);
}

void CoreSetFbneoStopCallback(CoreFbneoStopCallback callback, void* user)
{
    s_fbneoStopCallback = callback;
    s_fbneoStopUser     = user;
}

void CoreSetFbneoLocalInput(int playerIndex, uint32_t mask)
{
    fbneo_rt::SetLocalInput(playerIndex, mask);
}

uint32_t CoreGetFbneoLocalInput(int playerIndex)
{
    return fbneo_rt::GetLocalInput(playerIndex);
}

int CoreGetFbneoPlayerInputCount(int playerIndex)
{
    return fbneo_rt::GetPlayerInputCount(playerIndex);
}

const char* CoreGetFbneoPlayerInputName(int playerIndex, int bit)
{
    return fbneo_rt::GetPlayerInputName(playerIndex, bit);
}

bool CoreGetFbneoDriverInputNames(const std::string& gameName,
                                  std::vector<std::vector<std::string> >& outPerPlayer)
{
    return fbneo_rt::DriverInputNames(gameName.c_str(), outPerPlayer);
}

bool CoreGetFbneoRomInfo(const std::filesystem::path& file, std::string& shortName, std::string& fullName, std::string& romId)
{
    if (!fbneo_is_driver_archive(file))
        return false;

    std::string stem = file.stem().string();
    if (!fbneo_rt::DriverFullName(stem.c_str(), fullName))
        return false;

    shortName = stem;
    fbneo_rt::DriverRomId(stem.c_str(), romId);
    return true;
}

std::string CoreGetFbneoDriverName(void)
{
    if (!s_FbneoSessionActive)
        return std::string();

    return fbneo_rt::DriverShortName();
}

int CoreGetFbneoDipGroupCount(void)
{
    if (!s_FbneoSessionActive)
        return 0;
    return fbneo_rt::GetDipGroupCount();
}

bool CoreGetFbneoDipGroup(int group, std::string& name, int& settingCount, int& selectedSetting)
{
    if (!s_FbneoSessionActive)
        return false;
    fbneo_rt::DipGroupInfo info;
    if (!fbneo_rt::GetDipGroup(group, info))
        return false;
    name            = info.name;
    settingCount    = info.settingCount;
    selectedSetting = info.selectedSetting;
    return true;
}

bool CoreGetFbneoDipSettingName(int group, int setting, std::string& outName)
{
    if (!s_FbneoSessionActive)
        return false;
    return fbneo_rt::GetDipSettingName(group, setting, outName);
}

bool CoreSetFbneoDip(int group, int setting)
{
    if (!s_FbneoSessionActive)
        return false;
#ifdef NETPLAY
    // DIP changes mutate live driver state; keep them out of synchronized
    // rollback sessions where both peers must stay bit-identical.
    if (CoreIsSynchronizedNetplayActive())
        return false;
#endif
    if (!fbneo_rt::SetDipSelection(group, setting))
        return false;
    // Persist the (group, setting) choice so it is re-applied next session.
    fbneo_persist_dip_selection(fbneo_rt::DriverShortName(), group, setting);
    return true;
}

void CoreResetFbneoDips(void)
{
    if (!s_FbneoSessionActive)
        return;
#ifdef NETPLAY
    if (CoreIsSynchronizedNetplayActive())
        return;
#endif
    fbneo_rt::ResetDipsToDefaults();
    fbneo_clear_dip_settings(fbneo_rt::DriverShortName());
}

void CoreSetFbneoDipOverrides(const std::vector<std::pair<int, uint8_t> >& overrides)
{
    fbneo_rt::SetDipOverrides(overrides);
}

void CoreGetFbneoDipOverrides(std::vector<std::pair<int, uint8_t> >& out)
{
    fbneo_rt::GetDipOverrides(out);
}

bool CoreStartEmulation(std::filesystem::path n64rom, std::filesystem::path n64ddrom,
    std::string address, int port, int player)
{
    std::string error;
    m64p_error  m64p_ret = M64ERR_SUCCESS;
    bool        netplay_ret = false;
    CoreRomType type;
    bool        netplay = !address.empty();

#ifdef NETPLAY
    // Apply RSP plugin override and reload plugins BEFORE ROM open
    if (netplay && (address == "KAILLERA" || address.rfind("GEKKO|", 0) == 0 || address.rfind("LOBBY|", 0) == 0))
    {
        apply_kaillera_rsp_override();
        CoreApplyPluginSettings();  // Force reload with HLE RSP
    }
#endif

    // FBNeo romsets bypass the mupen64plus N64 ROM gate entirely: CoreOpenRom
    // (and the plugin / cheat / rom-type steps that need an open N64 ROM) reject
    // arcade archives, whose contents carry no N64 header.
    const bool fbneoTarget = fbneo_is_driver_archive(n64rom);
    s_FbneoSessionActive = fbneoTarget;

    if (!fbneoTarget)
    {
        if (!CoreOpenRom(n64rom))
        {
            return false;
        }

        if (!CoreApplyRomPluginSettings())
        {
            CoreApplyPluginSettings();
            CoreCloseRom();
            return false;
        }

        if (!CoreArePluginsReady())
        {
            CoreApplyPluginSettings();
            CoreCloseRom();
            return false;
        }

        if (!CoreAttachPlugins())
        {
            CoreApplyPluginSettings();
            CoreCloseRom();
            return false;
        }

        if (netplay)
        { // netplay cheats
            if (!CoreApplyNetplayCheats())
            {
                CoreDetachPlugins();
                CoreApplyPluginSettings();
                CoreCloseRom();
                return false;
            }
        }
        else
        { // local cheats
            if (!CoreApplyCheats())
            {
                CoreDetachPlugins();
                CoreApplyPluginSettings();
                CoreCloseRom();
                return false;
            }
        }

        if (!CoreGetRomType(type))
        {
            CoreClearCheats();
            CoreDetachPlugins();
            CoreApplyPluginSettings();
            CoreCloseRom();
            return false;
        }

        // set disk file in media loader when ROM is a cartridge
        if (type == CoreRomType::Cartridge)
        {
            CoreMediaLoaderSetDiskFile(n64ddrom);
        }

        // apply core settings overlay
        apply_coresettings_overlay();

        // apply game core settings overrides
        apply_game_coresettings_overlay();

        // apply pif rom settings
        apply_pif_rom_settings();
    }

    // A bare arcade romset is played through the local FBNeo session even when
    // the explicit "local rollback testing" toggle is off.
    const bool localRollbackEnabled = !netplay &&
        (CoreSettingsGetBoolValue(SettingsID::Rollback_EnableLocalTesting) || fbneoTarget);

    // FBNeo backend: resolve the driver name and archive list once for every
    // session start path below (P2P, lobby, local rollback).
    const std::string fbneoGame = fbneo_game_name(n64rom);
    const std::vector<std::string> fbneoArchiveStore = fbneo_rom_archives(n64rom);
    const std::vector<const char*> fbneoArchives = fbneo_archive_ptrs(fbneoArchiveStore);
    const int fbneoArchiveCount = static_cast<int>(fbneoArchives.size());
    const char* const* fbneoArchivePtrs = fbneoArchives.empty() ? nullptr : fbneoArchives.data();

    CoreAddCallbackMessage(CoreDebugMessageType::Info,
        std::string("CoreStartEmulation: fbneoTarget=") + (fbneoTarget ? "yes" : "no") +
        " rom=" + n64rom.filename().string() +
        (fbneoTarget ? (" driver=" + fbneoGame + " archives=" + std::to_string(fbneoArchiveCount)) : ""));

#ifdef NETPLAY
    // Apply deterministic settings AFTER all overlays for synchronized netplay or
    // explicit local rollback testing.
    if (localRollbackEnabled || (netplay && (address == "KAILLERA" || address.rfind("GEKKO|", 0) == 0 || address.rfind("LOBBY|", 0) == 0)))
    {
        apply_kaillera_deterministic_settings();
    }

    // Kaillera connection happens BEFORE emulation via kailleraSelectServerDialog
    // Just verify it's initialized if netplay was requested
    if (netplay)
    {
        // Check if address is "KAILLERA" marker (set by UI when using Kaillera)
        if (address == "KAILLERA")
        {
            if (!CoreHasInitKaillera())
            {
                CoreSetError("CoreStartEmulation: Kaillera not initialized");
                m64p_ret = M64ERR_SYSTEM_FAIL;
                netplay_ret = false;
            }
            else
            {
                // Store player number for input plugin to use
                CoreSetKailleraPlayerNumber(player);
                netplay_ret = true;
            }
        }
        else if (address.rfind("GEKKO|", 0) == 0)
        {
            std::string remoteAddress;
            int remotePort = 0;
            int frameDelay = 0;
            int predictionWindow = 7;
            if (!parse_gekko_address(address, remoteAddress, remotePort, frameDelay, predictionWindow))
            {
                CoreSetError("CoreStartEmulation: invalid GekkoNet session parameters");
                m64p_ret = M64ERR_INPUT_INVALID;
                netplay_ret = false;
            }
            else if (!rmgk_gekko::set_deterministic(true))
            {
                m64p_ret = M64ERR_SYSTEM_FAIL;
                netplay_ret = false;
            }
            else
            {
                CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, 2);
                if (!fbneo_rt::Initialize(fbneoGame.c_str(), fbneoArchivePtrs, fbneoArchiveCount, 2))
                {
                    const std::string fbErr = CoreGetError();
                    CoreSetError("CoreStartEmulation: FBNeo initialization failed" + (fbErr.empty() ? std::string() : (": " + fbErr)));
                    m64p_ret = M64ERR_SYSTEM_FAIL;
                    netplay_ret = false;
                }
                else
                {
                    s_FbneoActive = true;
                    netplay_ret = rmgk_gekko::start_p2p_session("rmgk-gekko",
                        2, 4, player, static_cast<unsigned short>(port),
                        remoteAddress.c_str(), static_cast<unsigned short>(remotePort), frameDelay, predictionWindow);
                    if (!netplay_ret)
                    {
                        if (CoreGetError().empty())
                        {
                            CoreSetError("CoreStartEmulation: GekkoNet session initialization failed");
                        }
                        m64p_ret = M64ERR_SYSTEM_FAIL;
                    }
                }
            }
        }
        else if (address.rfind("LOBBY|", 0) == 0)
        {
            int frameDelay = 0;
            int predictionWindow = 4;
            std::vector<LobbyRemotePeer> remotes;
            if (!parse_lobby_address(address, frameDelay, predictionWindow, remotes))
            {
                CoreSetError("CoreStartEmulation: invalid Lobby session parameters");
                m64p_ret = M64ERR_INPUT_INVALID;
                netplay_ret = false;
            }
            else if (!rmgk_gekko::set_deterministic(true))
            {
                m64p_ret = M64ERR_SYSTEM_FAIL;
                netplay_ret = false;
            }
            else
            {
                CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, 2);
                int totalSeats = player;
                for (const auto& remote : remotes)
                {
                    if (remote.slot > totalSeats)
                        totalSeats = remote.slot;
                }
                if (!fbneo_rt::Initialize(fbneoGame.c_str(), fbneoArchivePtrs, fbneoArchiveCount, totalSeats))
                {
                    const std::string fbErr = CoreGetError();
                    CoreSetError("CoreStartEmulation: FBNeo initialization failed" + (fbErr.empty() ? std::string() : (": " + fbErr)));
                    m64p_ret = M64ERR_SYSTEM_FAIL;
                    netplay_ret = false;
                }
                else
                {
                    s_FbneoActive = true;
                    netplay_ret = rmgk_gekko::start_lobby_session("rmgk-gekko",
                        totalSeats, 4,
                        player, static_cast<unsigned short>(port),
                        remotes.data(), static_cast<int>(remotes.size()),
                        frameDelay, predictionWindow);
                    if (!netplay_ret)
                    {
                        if (CoreGetError().empty())
                        {
                            CoreSetError("CoreStartEmulation: GekkoNet session initialization failed");
                        }
                        m64p_ret = M64ERR_SYSTEM_FAIL;
                    }
                }
            }
        }
        else
        {
            // Legacy netplay (Mupen64Plus built-in)
            netplay_ret = CoreInitNetplay(address, port, player);
            if (!netplay_ret)
            {
                m64p_ret = M64ERR_SYSTEM_FAIL;
            }
        }
    }
#endif // NETPLAY

    bool rollbackExecute = false;
    if (localRollbackEnabled)
    {
        const bool det = rmgk_gekko::set_deterministic(true);
        fprintf(stderr, "[FBNEO-DBG] local block enter det=%d\n", (int)det); fflush(stderr);
        if (!det)
        {
            m64p_ret = M64ERR_SYSTEM_FAIL;
        }
        else
        {
            CoreSettingsSetValue(SettingsID::Core_CPU_Emulator, 2);
            fprintf(stderr, "[FBNEO-DBG] calling fbneo_rt::Initialize game=%s archives=%d\n", fbneoGame.c_str(), fbneoArchiveCount); fflush(stderr);
            const bool fbInit = fbneo_rt::Initialize(fbneoGame.c_str(), fbneoArchivePtrs, fbneoArchiveCount, 4);
            fprintf(stderr, "[FBNEO-DBG] fbneo_rt::Initialize=%d err=%s\n", (int)fbInit, CoreGetError().c_str()); fflush(stderr);
            if (!fbInit)
            {
                const std::string fbErr = CoreGetError();
                CoreSetError("CoreStartEmulation: FBNeo initialization failed" + (fbErr.empty() ? std::string() : (": " + fbErr)));
                m64p_ret = M64ERR_SYSTEM_FAIL;
                netplay_ret = false;
            }
            else
            {
                s_FbneoActive = true;
                // Local (non-netplay) session: re-apply the user's saved DIP
                // choices on top of the driver defaults. Netplay sessions skip
                // this (guarded by localRollbackEnabled = !netplay) so peers match.
                fbneo_load_dip_settings(fbneo_rt::DriverShortName());
                netplay_ret = rmgk_gekko::start_local_session("rmgk-gekko-local",
                    4, 4, 0);
            rollbackExecute = netplay_ret;
            fprintf(stderr, "[FBNEO-DBG] start_local_session=%d err=%s\n", (int)netplay_ret, CoreGetError().c_str()); fflush(stderr);
            if (!netplay_ret)
            {
                if (CoreGetError().empty())
                {
                    CoreSetError("CoreStartEmulation: local GekkoNet session initialization failed");
                }
                m64p_ret = M64ERR_SYSTEM_FAIL;
            }
            else
            {
                // Session is up; drop any benign error latched during setup (e.g.
                // a probe for a mupen input-plugin parameter that doesn't apply).
                CoreSetError(std::string());
            }
        }
    }
    }

    // FBNeo P2P/lobby GekkoNet sessions are driven by the rollback frame loop;
    // there is no N64 ROM open, so the mupen64plus M64CMD_EXECUTE must not run
    // for them (it would fail with a missing config parameter).
    if (netplay && netplay_ret && fbneoTarget)
    {
        rollbackExecute = true;
    }

    // only start emulation when initializing netplay/local rollback
    // is successful or if there's legacy netplay requested
    if ((!netplay && (!localRollbackEnabled || rollbackExecute)) || (netplay && netplay_ret))
    {
        // Register frame callback for frame counter (used by Kaillera)
        s_CurrentFrame = 0;
        m64p::Core.DoCommand(M64CMD_SET_FRAME_CALLBACK, 0, (void*)FrameCallback);

#ifdef NETPLAY
        // Reset Kaillera sync state to prevent stale cache from previous sessions
        s_LastSyncFrame = -1;
        s_SyncedThisFrame = false;
        s_CachedNumReceived = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            s_CachedSyncBuffer[i] = 0;
        }
#endif

#ifdef NETPLAY
        // Register Kaillera PIF sync callback (works with any input plugin)
        // Get function pointer dynamically since mupen64plus is loaded at runtime
        typedef void (*set_pif_sync_callback_t)(pif_sync_callback_t);
        void* coreHandle = m64p::Core.GetHandle();
        if (coreHandle)
        {
#ifdef _WIN32
            set_pif_sync_callback_t set_callback =
                (set_pif_sync_callback_t)GetProcAddress((HMODULE)coreHandle, "set_pif_sync_callback");
#else
            set_pif_sync_callback_t set_callback =
                (set_pif_sync_callback_t)dlsym(coreHandle, "set_pif_sync_callback");
#endif
            if (set_callback)
            {
                set_callback(address == "KAILLERA" ? KailleraPifSyncCallback : nullptr);
            }
        }
#endif

        CoreRollbackSetVerboseStats(CoreSettingsGetBoolValue(SettingsID::Rollback_VerboseStats));
        setRollbackLoggingEnvironment();

        if (rollbackExecute)
        {
            m64p_ret = rmgk_gekko::execute() ? M64ERR_SUCCESS : M64ERR_SYSTEM_FAIL;
        }
        else
        {
            m64p_ret = m64p::Core.DoCommand(M64CMD_EXECUTE, 0, nullptr);
        }
        if (rollbackExecute && fbneoTarget && rmgk_gekko::is_stop_requested())
        {
            // A user-initiated stop of an arcade (FBNeo) session ends the frame
            // loop with a "failure" return from execute(); that is the normal
            // exit path, not an error. Discard any error latched during the
            // session so it isn't surfaced on shutdown.
            m64p_ret = M64ERR_SUCCESS;
            CoreSetError(std::string());
        }
        else if (m64p_ret != M64ERR_SUCCESS)
        {
            error = rollbackExecute ?
                "CoreStartEmulation rollback execute Failed: " :
                "CoreStartEmulation m64p::Core.DoCommand(M64CMD_EXECUTE) Failed: ";
            if (!CoreGetError().empty())
            {
                error += CoreGetError();
            }
            else
            {
                error += m64p::Core.ErrorMessage(m64p_ret);
            }

            // A plugin fatal error at startup is almost always the video plugin
            // failing to initialize â€” most often an OpenGL version / driver
            // problem (the raw message is opaque). Point the user at the cause.
            if (error.find("plugin function returned a fatal error") != std::string::npos)
            {
                error += "  This usually means the graphics plugin couldn't start â€” "
                         "often an outdated or unsupported graphics driver (GLideN64 needs "
                         "OpenGL 3.3+). Update your GPU driver, or pick a different video "
                         "plugin in Settings.";
            }
        }
    }

    // The frontend tears down its FBNeo video/audio output on the emulation
    // thread right after the frame loop ends, while its GL context is still
    // current.
    if (rollbackExecute && s_fbneoStopCallback)
    {
        s_fbneoStopCallback(s_fbneoStopUser);
    }

    // FBNeo was initialized on this emulation thread and must only be torn down
    // here, after the rollback frame loop has fully returned. Tearing it down from
    // CoreStopEmulation (the GUI thread) raced the loop and made the final
    // state-save fail.
    if (s_FbneoActive)
    {
        fbneo_rt::Shutdown();
        s_FbneoActive = false;
    }

    // Once the frame loop has returned the arcade session is over; clear the flag
    // so a later CoreStopEmulation() (e.g. app shutdown) doesn't take the FBNeo
    // early-return path for a session that no longer exists.
    s_FbneoSessionActive = false;

    if (!netplay && rollbackExecute)
    {
        rmgk_gekko::close_session();
    }

#ifdef NETPLAY
    if (netplay && netplay_ret)
    {
        // Check if we used Kaillera or legacy netplay
        if (address == "KAILLERA")
        {
            // Don't shutdown Kaillera here - keep connection alive for restart
            // Kaillera will be shutdown when user leaves the server dialog
        }
        else if (address.rfind("GEKKO|", 0) == 0 || address.rfind("LOBBY|", 0) == 0)
        {
            rmgk_gekko::close_session();
        }
        else
        {
            CoreShutdownNetplay();
        }
    }
#endif // NETPLAY

    // Arcade (FBNeo) sessions never opened an N64 ROM or attached the mupen64plus
    // plugins, so the ROM/plugin teardown must be skipped (it would fail with
    // "cannot close rom when no rom is open!").
    if (!fbneoTarget)
    {
        CoreClearCheats();
        CoreDetachPlugins();
        CoreCloseRom();
    }

    // restore plugin settings
    CoreApplyPluginSettings();

    // reset media loader state
    CoreResetMediaLoader();

    if (!netplay || netplay_ret)
    {
        // we need to set the emulation error last,
        // to prevent the other functions from
        // overriding the emulation error
        CoreSetError(error);
    }

    return m64p_ret == M64ERR_SUCCESS;
}

bool CoreStopEmulation(void)
{
    std::string error;
    m64p_error ret;

#ifdef NETPLAY
    rmgk_gekko::request_stop();
#endif

    // Arcade (FBNeo) sessions are driven by the rollback frame loop, never by the
    // mupen64plus core, so M64CMD_STOP would fail with INVALID_STATE. The
    // request_stop() above asks the loop to exit; the emulation thread then tears
    // FBNeo down in CoreStartEmulation once the loop has returned.
    if (s_FbneoSessionActive)
    {
        return true;
    }

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_STOP, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreStopEmulation m64p::Core.DoCommand(M64CMD_STOP) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
        return false;
    }

#ifdef NETPLAY
    // Clear Kaillera player number when stopping
    CoreSetKailleraPlayerNumber(0);
#endif

    return ret == M64ERR_SUCCESS;
}

bool CorePauseEmulation(void)
{
    std::string error;
    m64p_error ret;

    // Arcade (FBNeo) sessions don't run the mupen64plus core, so the
    // M64CMD_PAUSE path below can't work. Toggle the FBNeo runtime's pause
    // flag instead.
    if (s_FbneoSessionActive)
    {
        if (CoreIsSynchronizedNetplayActive())
        {
            return false;
        }

        if (!CoreIsEmulationRunning())
        {
            error = "CorePauseEmulation Failed: ";
            error += "cannot pause emulation when emulation isn't running!";
            CoreSetError(error);
            return false;
        }

        fbneo_rt::SetPaused(true);
        return true;
    }

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreIsSynchronizedNetplayActive())
    {
        return false;
    }

    if (!CoreIsEmulationRunning())
    {
        error = "CorePauseEmulation Failed: ";
        error += "cannot pause emulation when emulation isn't running!";
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_PAUSE, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CorePauseEmulation m64p::Core.DoCommand(M64CMD_PAUSE) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

bool CoreResumeEmulation(void)
{
    std::string error;
    m64p_error ret;

    // Arcade (FBNeo) sessions don't run the mupen64plus core, so the
    // M64CMD_RESUME path below can't work. Clear the FBNeo runtime's pause
    // flag instead.
    if (s_FbneoSessionActive)
    {
        if (CoreIsSynchronizedNetplayActive())
        {
            return false;
        }

        if (!CoreIsEmulationPaused())
        {
            error = "CoreResumeEmulation Failed: ";
            error += "cannot resume emulation when emulation isn't paused!";
            CoreSetError(error);
            return false;
        }

        fbneo_rt::SetPaused(false);
        return true;
    }

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreIsSynchronizedNetplayActive())
    {
        return false;
    }

    if (!CoreIsEmulationPaused())
    {
        error = "CoreIsEmulationPaused Failed: ";
        error += "cannot resume emulation when emulation isn't paused!";
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_RESUME, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreResumeEmulation m64p::Core.DoCommand(M64CMD_RESUME) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

bool CoreAdvanceFrame(void)
{
    return CoreAdvanceFrames(1);
}

bool CoreAdvanceFrames(int frames)
{
    return CoreRunFrames(frames, CoreFrameOutput_All);
}

bool CoreRunFrames(int frames, int flags)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreIsSynchronizedNetplayActive())
    {
        return false;
    }

    if (!CoreIsEmulationRunning() && !CoreIsEmulationPaused())
    {
        return false;
    }

    if (frames < 1)
    {
        frames = 1;
    }

    if (s_FbneoActive)
    {
        for (int i = 0; i < frames; i++)
        {
            if (!fbneo_rt::RunFrame(flags))
            {
                CoreSetError("CoreRunFrames fbneo_rt::RunFrame Failed");
                return false;
            }
        }
        return true;
    }

    ret = m64p::Core.DoCommand(M64CMD_RUN_FRAMES, frames, &flags);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreRunFrames DoCommand(M64CMD_RUN_FRAMES) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

bool CoreSetFrameOutput(int flags)
{
    std::string error;
    m64p_error ret;

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_FRAME_OUTPUT_SET, flags, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreSetFrameOutput DoCommand(M64CMD_FRAME_OUTPUT_SET) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

bool CoreResetEmulation(bool hard)
{
    std::string error;
    m64p_error ret;

    // Arcade (FBNeo) sessions don't run the mupen64plus core, so the M64CMD_RESET
    // path below can't work. Re-initialize the FBNeo driver instead.
    if (s_FbneoSessionActive)
    {
        if (CoreIsSynchronizedNetplayActive())
        {
            CoreSetError("CoreResetEmulation Failed: cannot reset during a synchronized netplay session!");
            return false;
        }

        if (!fbneo_rt::Reset())
        {
            if (CoreGetError().empty())
            {
                CoreSetError("CoreResetEmulation: FBNeo reset failed");
            }
            return false;
        }

        return true;
    }

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    if (CoreIsEmulationPaused())
    {
        error = "CoreResetEmulation Failed: ";
        error += "cannot reset emulation when paused!";
        CoreSetError(error);
        return false;
    }

    if (!CoreIsEmulationRunning())
    {
        error = "CoreResetEmulation Failed: ";
        error += "cannot reset emulation when emulation isn't running!";
        CoreSetError(error);
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_RESET, hard, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreResetEmulation m64p::Core.DoCommand(M64CMD_RESET) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

bool CoreIsEmulationRunning(void)
{
    if (s_FbneoActive)
        return true;
    m64p_emu_state state = M64EMU_STOPPED;
    return get_emulation_state(state) && state == M64EMU_RUNNING;
}

bool CoreIsEmulationPaused(void)
{
    if (s_FbneoActive)
        return fbneo_rt::IsPaused();
    m64p_emu_state state = M64EMU_STOPPED;
    return get_emulation_state(state) && state == M64EMU_PAUSED;
}

bool CoreIsFbneoSessionActive(void)
{
    return s_FbneoSessionActive;
}

bool CoreIsSynchronizedNetplayActive(void)
{
    if (CoreHasInitNetplay())
    {
        return true;
    }

    if (CoreHasInitKaillera() && !CoreIsKailleraPlaybackMode())
    {
        return true;
    }

#ifdef NETPLAY
    if (rmgk_gekko::is_netplay_session_active())
    {
        return true;
    }
#endif

    return false;
}

int CoreGetCurrentFrameCount(void)
{
    // Return frame counter updated via frame callback
    return s_CurrentFrame;
}
