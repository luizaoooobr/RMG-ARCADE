/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef ADAPTER_HPP
#define ADAPTER_HPP

#include <cstdint>

struct GameCubeAdapterControllerState
{
    uint8_t Status;

    union
    {
        uint8_t Buttons1;
        struct
        {
            bool A : 1;
            bool B : 1;
            bool X : 1;
            bool Y : 1;

            bool DpadLeft  : 1;
            bool DpadRight : 1;
            bool DpadDown  : 1;
            bool DpadUp    : 1;
        };
    };

    union
    {
        uint8_t Buttons2;
        struct
        {
            bool Start : 1;
            bool Z : 1;
            bool R : 1;
            bool L : 1;
        };
    };

    uint8_t LeftStickX;
    uint8_t LeftStickY;
    uint8_t RightStickX;
    uint8_t RightStickY;

    uint8_t LeftTrigger;
    uint8_t RightTrigger;
};

// Start adapter polling for configuration UI (if not already running)
bool GCA_StartConfigPolling(void);

// Stop adapter polling started by configuration UI
void GCA_StopConfigPolling(void);

// Get current controller state (thread-safe)
GameCubeAdapterControllerState GCA_GetControllerState(int port);

#endif // ADAPTER_HPP
