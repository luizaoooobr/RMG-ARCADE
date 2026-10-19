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
#include "Directories.hpp"
#include "RomSettings.hpp"
#include "RomHeader.hpp"
#include "SaveState.hpp"
#include "Settings.hpp"
#include "Library.hpp"
#include "Error.hpp"
#include "FbneoRuntime.hpp"

#include "m64p/Api.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

//
// Local Variables
//

static bool l_HasRollbackState = false;

// Save-state slot used by arcade (FBNeo) sessions. There is no mupen64plus core
// to track a slot for them, so it is kept here.
static int l_FbneoSaveStateSlot = 0;

//
// Local Functions
//

// replaces all occurences of any chars in replace with c inside str
static void str_replace_chars(std::string& str, const std::string replace, const char c)
{
    for (size_t i = 0; i < str.size(); i++)
    {
        char str_char = str.at(i);
        if (replace.find(str_char) != std::string::npos)
        {
            str.at(i) = c;
        }
    }
}

// Path used for arcade (FBNeo) quick save states: <savestate dir>/<driver>.st<slot>.
// There is no mupen64plus ROM header/settings for arcade sessions, so the driver
// short name stands in for the game identity.
static bool fbneo_save_state_path(int slot, std::filesystem::path& path)
{
    if (!fbneo_rt::IsInitialized())
    {
        return false;
    }

    const char* driver = fbneo_rt::DriverShortName();
    if (driver == nullptr || *driver == '\0')
    {
        return false;
    }

    std::string fileName = driver;
    str_replace_chars(fileName, ":<>\"/\\|?*", '_');
    fileName += ".st";
    fileName += std::to_string(slot);

    path = CoreGetSaveStateDirectory();
    path += CORE_DIR_SEPERATOR_STR;
    path += fileName;
    return true;
}

static bool fbneo_write_state(const std::filesystem::path& path)
{
    unsigned char* buffer = nullptr;
    int len = 0;
    int checksum = 0;

    if (!fbneo_rt::SaveStateAlloc(buffer, len, checksum, 0) || buffer == nullptr || len <= 0)
    {
        CoreSetError("CoreSaveState: FBNeo state serialization failed");
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        std::free(buffer);
        CoreSetError("CoreSaveState: couldn't open \"" + path.string() + "\" for writing");
        return false;
    }

    out.write(reinterpret_cast<const char*>(buffer), static_cast<std::streamsize>(len));
    const bool ok = out.good();
    out.close();
    std::free(buffer);

    if (!ok)
    {
        CoreSetError("CoreSaveState: failed to write \"" + path.string() + "\"");
    }

    return ok;
}

static bool fbneo_read_state(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        CoreSetError("CoreLoadSaveState: couldn't open \"" + path.string() + "\"");
        return false;
    }

    std::vector<unsigned char> buffer((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
    if (buffer.empty())
    {
        CoreSetError("CoreLoadSaveState: save state file is empty");
        return false;
    }

    if (!fbneo_rt::LoadState(buffer.data(), static_cast<int>(buffer.size())))
    {
        CoreSetError("CoreLoadSaveState: FBNeo state deserialization failed");
        return false;
    }

    return true;
}

//
// Exported Functions
//

CORE_EXPORT bool CoreSetSaveStateSlot(int slot)
{
    std::string error;
    m64p_error ret;

    if (fbneo_rt::IsInitialized())
    {
        l_FbneoSaveStateSlot = slot;
        return true;
    }

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_CORE_STATE_SET, M64CORE_SAVESTATE_SLOT, &slot);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreSetSaveStateSlot m64p::Core.DoCommand(M64CMD_CORE_STATE_SET, M64CORE_SAVESTATE_SLOT) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT int CoreGetSaveStateSlot(void)
{
    std::string error;
    m64p_error ret;
    int slot = -1;

    if (fbneo_rt::IsInitialized())
    {
        return l_FbneoSaveStateSlot;
    }

    if (!m64p::Core.IsHooked())
    {
        return -1;
    }

    ret = m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY, M64CORE_SAVESTATE_SLOT, &slot);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreGetSaveStateSlot m64p::Core.DoCommand(M64CMD_CORE_STATE_QUERY, M64CORE_SAVESTATE_SLOT) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return slot;
}

CORE_EXPORT bool CoreIncreaseSaveStateSlot(void)
{
    std::string error;
    int slot;

    if (!m64p::Core.IsHooked() && !fbneo_rt::IsInitialized())
    {
        return false;
    }

    slot = CoreGetSaveStateSlot();
    if (slot >= 9)
    {
        error = "CoreIncreaseSaveStateSlot Failed: cannot increase save state slot!";
        CoreSetError(error);
        return false;
    }

    return CoreSetSaveStateSlot(slot + 1);
}

CORE_EXPORT bool CoreDecreaseSaveStateSlot(void)
{
    std::string error;
    int slot;

    if (!m64p::Core.IsHooked() && !fbneo_rt::IsInitialized())
    {
        return false;
    }

    slot = CoreGetSaveStateSlot();
    if (slot <= 0)
    {
        error = "CoreDecreaseSaveStateSlot Failed: cannot decrease save state slot!";
        CoreSetError(error);
        return false;
    }

    return CoreSetSaveStateSlot(slot - 1);
}

CORE_EXPORT bool CoreGetSaveStatePath(CoreRomHeader header, CoreRomSettings settings, int slot, std::filesystem::path& path)
{
    // TODO: this should probably be an API function
    // in mupen64plus-core instead
    std::filesystem::path saveStatePath;
    std::filesystem::path oldSaveStatePath;
    std::string saveStateFileName;
    std::filesystem::path saveStateExtension;

    // retrieve save state directory
    saveStatePath = CoreGetSaveStateDirectory();
    saveStatePath += CORE_DIR_SEPERATOR_STR;

    // construct extension
    saveStateExtension = ".st";
    saveStateExtension += std::to_string(slot);

    // construct old save state path
    oldSaveStatePath = saveStatePath;
    oldSaveStatePath += settings.GoodName;
    oldSaveStatePath += saveStateExtension;

    // use old filename if it exists
    if (std::filesystem::is_regular_file(oldSaveStatePath))
    {
        path = oldSaveStatePath;
        return true;
    }

    // else use new filename
    int format = CoreSettingsGetIntValue(SettingsID::Core_SaveFileNameFormat);
    if (format == 0)
    {
        saveStatePath += header.Name;
        saveStatePath += saveStateExtension;
    }
    else
    {
        if (settings.GoodName.find("(unknown rom)") == std::string::npos)
        {
            if (settings.GoodName.size() < 32)
            {
                saveStatePath += settings.GoodName;
            }
            else
            {
                saveStatePath += settings.GoodName.substr(0, 32);
            }
        }
        else if (!header.Name.empty())
        {
            saveStatePath += header.Name;
        }
        else
        {
            saveStatePath += "unknown";
        }
        saveStatePath += "-";
        saveStatePath += settings.MD5.substr(0, 8);
        saveStatePath += saveStateExtension;
    }

    // retrieve filename from path
    saveStateFileName = saveStatePath.filename().string();

    // sanitize filename
    str_replace_chars(saveStateFileName, ":<>\"/\\|?*", '_');

    // replace filename in path
    saveStatePath.replace_filename(saveStateFileName);

    path = saveStatePath;
    return true;
}

CORE_EXPORT bool CoreGetSaveStatePath(int slot, std::filesystem::path& path)
{
    if (fbneo_rt::IsInitialized())
    {
        return fbneo_save_state_path(slot, path);
    }

    CoreRomHeader romHeader;
    CoreRomSettings romSettings;

    // attempt to retrieve the current
    // rom header and settings
    if (!CoreGetCurrentRomHeader(romHeader) ||
        !CoreGetCurrentRomSettings(romSettings))
    {
        return false;
    }

    return CoreGetSaveStatePath(romHeader, romSettings, slot, path);
}

CORE_EXPORT bool CoreSaveState(void)
{
    std::string error;
    m64p_error ret;

    if (fbneo_rt::IsInitialized())
    {
        std::filesystem::path path;
        if (!fbneo_save_state_path(CoreGetSaveStateSlot(), path))
        {
            CoreSetError("CoreSaveState: couldn't resolve the FBNeo save state path");
            return false;
        }
        return fbneo_write_state(path);
    }

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_STATE_SAVE, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreSaveState: m64p::Core.DoCommand(M64CMD_STATE_SAVE) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreSaveState(std::filesystem::path file, CoreSaveStateType type)
{
    std::string error;
    m64p_error ret;

    if (fbneo_rt::IsInitialized())
    {
        return fbneo_write_state(file);
    }

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_STATE_SAVE, static_cast<int>(type), const_cast<char*>(file.string().c_str()));
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreSaveState: m64p::Core.DoCommand(M64CMD_STATE_SAVE) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreLoadSaveState(void)
{
    std::string error;
    m64p_error ret;

    if (fbneo_rt::IsInitialized())
    {
        std::filesystem::path path;
        if (!fbneo_save_state_path(CoreGetSaveStateSlot(), path))
        {
            CoreSetError("CoreLoadSaveState: FBNeo save state path unavailable");
            return false;
        }
        return fbneo_read_state(path);
    }

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_STATE_LOAD, 0, nullptr);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreLoadSaveState: m64p::Core.DoCommand(M64CMD_STATE_LOAD) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreLoadSaveState(std::filesystem::path file)
{
    std::string error;
    m64p_error ret;

    if (fbneo_rt::IsInitialized())
    {
        return fbneo_read_state(file);
    }

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_STATE_LOAD, 0, const_cast<char*>(file.string().c_str()));
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreLoadSaveState: m64p::Core.DoCommand(M64CMD_STATE_LOAD) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreSaveRollbackState(void)
{
    std::string error;
    m64p_error ret;
    static char memoryState[] = "MEMORY";

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_STATE_SAVE, static_cast<int>(CoreSaveStateType::Mupen64Plus), memoryState);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreSaveRollbackState: m64p::Core.DoCommand(M64CMD_STATE_SAVE) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }
    else
    {
        l_HasRollbackState = true;
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreLoadRollbackState(void)
{
    std::string error;
    m64p_error ret;
    static char memoryState[] = "MEMORY";

    if (!m64p::Core.IsHooked())
    {
        return false;
    }

    ret = m64p::Core.DoCommand(M64CMD_STATE_LOAD, 0, memoryState);
    if (ret != M64ERR_SUCCESS)
    {
        error = "CoreLoadRollbackState: m64p::Core.DoCommand(M64CMD_STATE_LOAD) Failed: ";
        error += m64p::Core.ErrorMessage(ret);
        CoreSetError(error);
    }

    return ret == M64ERR_SUCCESS;
}

CORE_EXPORT bool CoreHasRollbackState(void)
{
    return m64p::Core.IsHooked() && l_HasRollbackState;
}
