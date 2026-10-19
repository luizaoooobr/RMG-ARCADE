/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef ARCADEINPUTDIALOG_HPP
#define ARCADEINPUTDIALOG_HPP

#include <QDialog>
#include <QList>
#include <QString>
#include <QTimer>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>

class QTabWidget;
class QComboBox;
class QTableWidget;
class QPushButton;
class QLabel;
class QKeyEvent;

namespace UserInterface
{
namespace Dialog
{
// One arcade set offered by the input dialog when no game is running, so the
// user can map a game's controls ahead of time. `driver` is the FBNeo short name
// (e.g. "sf2ce") used to key the per-driver bindings section.
struct ArcadeInputGame
{
    QString display;
    QString driver;
};

// Maps the arcade controls of the running FBNeo driver directly (FBNeo-style),
// replacing the N64-controller translation. Bindings are stored as flat parallel
// lists keyed by input name -- the exact data the emulation-thread input handler
// reads every frame (see RMG/VidExt.cpp).
//
// When no arcade game is running the dialog works in "pick a game" mode: it
// shows the supplied arcade sets, enumerates the selected driver's inputs (no
// emulation needed) and saves the bindings under that driver's section, so they
// are already in place when the game is played.
class ArcadeInputDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ArcadeInputDialog(QWidget* parent, const QList<ArcadeInputGame>& games = QList<ArcadeInputGame>());
    ~ArcadeInputDialog() override;

protected:
    void keyPressEvent(QKeyEvent* event) override;

private:
    // Mirrors the InputType values the runtime reads from settings.
    enum BindingSource
    {
        Source_Keyboard       = -1,
        Source_GamepadButton  = 0,
        Source_GamepadAxis    = 1,
        Source_JoystickButton = 2,
        Source_JoystickAxis   = 3,
        Source_JoystickHat    = 4,
    };

    struct Binding
    {
        int type  = 0;
        int data  = 0;
        int extra = 0;
    };

    struct DeviceOption
    {
        int         type = 0;
        std::string name;
        std::string path;
        int         num = -1;
    };

    struct PlayerPage
    {
        int                       player        = -1;
        QComboBox*                deviceCombo   = nullptr;
        QTableWidget*             table         = nullptr;
        QPushButton*              setButton     = nullptr;
        QPushButton*              clearButton   = nullptr;
        QLabel*                   statusLabel   = nullptr;
        std::vector<DeviceOption> deviceOptions;
        int                       listeningRow  = -1;
    };

    struct CaptureDevice
    {
        bool                isGamepad = false;
        SDL_Gamepad*        gamepad   = nullptr;
        SDL_Joystick*       joystick  = nullptr;
        std::vector<bool>   buttonPrev;
        std::vector<int>    axisPrev;
        std::vector<int>    hatPrev;
    };

    void buildUi(void);
    void buildTabs(void);
    void loadInputNames(void);
    void loadSettings(void);
    void saveSettings(void);
    void loadBindings(void);
    void saveBindings(void);
    void switchGame(const QString& driver);

    void        rebuildTables(void);
    void        updateRowDescription(PlayerPage& page, int row);

    void        startListening(PlayerPage& page, int row);
    void        stopListening(void);
    void        commitCapture(const Binding& binding);
    void        openCaptureDevices(void);
    void        closeCaptureDevices(void);
    bool        pollCaptureDevices(void);

    static std::string describeBinding(const Binding& binding);

    std::unordered_map<std::string, std::vector<Binding>> m_bindings;
    std::vector<std::vector<std::string>> m_inputNames;
    std::vector<PlayerPage>    m_pages;
    std::vector<CaptureDevice> m_captureDevices;

    QTabWidget*  m_tabWidget     = nullptr;
    QWidget*     m_tabsContainer = nullptr;
    QComboBox*   m_gameCombo     = nullptr;
    QLabel*      m_globalStatus  = nullptr;
    QString      m_driverName;
    QList<ArcadeInputGame> m_games;
    bool         m_pickGameMode  = false;
    std::string  m_section;
    QTimer*      m_captureTimer  = nullptr;

    PlayerPage*  m_listeningPage = nullptr;
    int          m_listeningRow  = -1;
    std::string  m_listeningName;
    Binding      m_capturedBinding;
};
} // namespace Dialog
} // namespace UserInterface

#endif // ARCADEINPUTDIALOG_HPP
