/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "ArcadeInputDialog.hpp"

#include <RMG-Core/Emulation.hpp>
#include <RMG-Core/Settings.hpp>

#include <QList>
#include <QString>
#include <QStringList>

#include "Utilities/QtKeyToSdl3Key.hpp"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QComboBox>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QDialogButtonBox>
#include <QIcon>
#include <QKeyEvent>
#include <QAbstractItemView>

#include <algorithm>
#include <cstdlib>

// Implemented in VidExt.cpp (same binary): re-reads the live FBNeo input mapping.
void FbneoInputReloadSettings(void);

using namespace UserInterface::Dialog;

namespace
{
constexpr int kAxisPeak = 32767;

// One connected joystick as listed in the device combo (enumerated once).
struct JoystickEntry
{
    std::string name;
    std::string path;
    int         num = -1;
};
} // namespace

ArcadeInputDialog::ArcadeInputDialog(QWidget* parent, const QList<ArcadeInputGame>& games)
    : QDialog(parent), m_games(games)
{
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setWindowTitle(tr("Arcade Input Settings"));
    setModal(true);
    setMinimumSize(640, 480);

    if (!SDL_WasInit(SDL_INIT_GAMEPAD))
    {
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    }

    // With a game running the dialog edits the live driver; otherwise it lets the
    // user pick an arcade set and configure that driver's controls ahead of time.
    m_pickGameMode = !CoreIsFbneoSessionActive();
    if (!m_pickGameMode)
    {
        m_driverName = QString::fromStdString(CoreGetFbneoDriverName());
    }
    else if (!m_games.isEmpty())
    {
        m_driverName = m_games.first().driver;
    }

    this->loadInputNames();
    this->loadBindings();

    m_captureTimer = new QTimer(this);
    m_captureTimer->setInterval(16);
    connect(m_captureTimer, &QTimer::timeout, this, [this]() {
        if (m_listeningPage == nullptr)
        {
            return;
        }
        if (this->pollCaptureDevices())
        {
            this->commitCapture(m_capturedBinding);
        }
    });

    this->buildUi();
    this->loadSettings();
    this->rebuildTables();
    adjustSize();
}

ArcadeInputDialog::~ArcadeInputDialog()
{
    this->closeCaptureDevices();
}

void ArcadeInputDialog::buildUi(void)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    m_globalStatus = new QLabel(tr("Select an input and press \"Set\" to bind a key or button."), this);
    m_globalStatus->setWordWrap(true);
    root->addWidget(m_globalStatus);

    // Pick-a-game selector, shown only when no arcade game is running. The user
    // maps a game's controls ahead of time; bindings are stored per driver.
    if (m_pickGameMode)
    {
        auto* gameRow = new QHBoxLayout();
        gameRow->addWidget(new QLabel(tr("Game:"), this));
        m_gameCombo = new QComboBox(this);
        for (const ArcadeInputGame& game : m_games)
        {
            const QString label = game.display.isEmpty() ? game.driver : game.display;
            m_gameCombo->addItem(label, game.driver);
        }
        const int current = m_gameCombo->findData(m_driverName);
        if (current >= 0)
        {
            m_gameCombo->setCurrentIndex(current);
        }
        gameRow->addWidget(m_gameCombo, 1);
        root->addLayout(gameRow);

        connect(m_gameCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
            if (index < 0)
            {
                return;
            }
            this->switchGame(m_gameCombo->itemData(index).toString());
        });
    }

    m_tabWidget = new QTabWidget(this);
    root->addWidget(m_tabWidget, 1);

    this->buildTabs();

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
        this->saveSettings();
        this->accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void ArcadeInputDialog::buildTabs(void)
{
    // (Re)build the per-player tabs for the current driver. Also invoked when the
    // user switches games, so tear the previous pages down first.
    m_listeningPage = nullptr;
    m_listeningRow  = -1;
    m_listeningName.clear();

    while (m_tabWidget->count() > 0)
    {
        QWidget* page = m_tabWidget->widget(0);
        m_tabWidget->removeTab(0);
        delete page;
    }
    m_pages.clear();

    // Enumerate connected joysticks once (no open) for the device combos.
    std::vector<JoystickEntry> joysticks;
    int                        jcount = 0;
    SDL_JoystickID*            jids   = SDL_GetJoysticks(&jcount);
    for (int i = 0; i < jcount; i++)
    {
        JoystickEntry entry;
        const char*   name = SDL_GetJoystickNameForID(jids[i]);
        const char*   path = SDL_GetJoystickPathForID(jids[i]);
        entry.name = (name != nullptr) ? name : std::string();
        entry.path = (path != nullptr) ? path : std::string();
        entry.num  = i;
        joysticks.push_back(entry);
    }
    if (jids != nullptr)
    {
        SDL_free(jids);
    }

    m_pages.reserve(4);
    for (int player = 0; player < 4; player++)
    {
        if (player >= static_cast<int>(m_inputNames.size()) ||
            m_inputNames[static_cast<size_t>(player)].empty())
        {
            continue;
        }

        m_pages.emplace_back();
        PlayerPage& page = m_pages.back();
        page.player = player;

        auto* pageWidget = new QWidget(m_tabWidget);
        auto* pageLayout = new QVBoxLayout(pageWidget);
        pageLayout->setContentsMargins(8, 8, 8, 8);
        pageLayout->setSpacing(8);

        // Device selector.
        auto* deviceRow = new QHBoxLayout();
        deviceRow->addWidget(new QLabel(tr("Device:"), pageWidget));
        page.deviceCombo = new QComboBox(pageWidget);
        page.deviceOptions.clear();
        {
            DeviceOption automatic;
            automatic.type = 1;
            automatic.num  = -1;
            page.deviceOptions.push_back(automatic);
            page.deviceCombo->addItem(tr("Automatic"));

            DeviceOption keyboard;
            keyboard.type = 2;
            keyboard.num  = -1;
            page.deviceOptions.push_back(keyboard);
            page.deviceCombo->addItem(tr("Keyboard"));

            for (const JoystickEntry& entry : joysticks)
            {
                DeviceOption option;
                option.type = 3;
                option.name = entry.name;
                option.path = entry.path;
                option.num  = entry.num;
                page.deviceOptions.push_back(option);

                QString label = QString::fromStdString(entry.name);
                if (label.isEmpty())
                {
                    label = tr("Joystick %1").arg(entry.num);
                }
                page.deviceCombo->addItem(label);
            }
        }
        deviceRow->addWidget(page.deviceCombo, 1);
        pageLayout->addLayout(deviceRow);

        // Inputs table.
        page.table = new QTableWidget(pageWidget);
        page.table->setColumnCount(2);
        page.table->setHorizontalHeaderLabels({ tr("Game input"), tr("Mapped to") });
        page.table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
        page.table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        page.table->verticalHeader()->setVisible(false);
        page.table->setSelectionMode(QAbstractItemView::SingleSelection);
        page.table->setSelectionBehavior(QAbstractItemView::SelectRows);
        page.table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        page.table->setAlternatingRowColors(true);
        // The main window's fallback stylesheet colours table text with the accent
        // blue; keep selected rows readable by forcing a contrasting selection.
        page.table->setStyleSheet(
            "QTableWidget::item:selected { color: #FFFFFF; background-color: #0096d3; }");
        pageLayout->addWidget(page.table, 1);

        // Buttons + per-page status.
        auto* buttonRow = new QHBoxLayout();
        page.setButton = new QPushButton(tr("Set"), pageWidget);
        page.clearButton = new QPushButton(tr("Clear"), pageWidget);
        buttonRow->addWidget(page.setButton);
        buttonRow->addWidget(page.clearButton);
        buttonRow->addStretch(1);
        page.statusLabel = new QLabel(tr("Ready"), pageWidget);
        buttonRow->addWidget(page.statusLabel);
        pageLayout->addLayout(buttonRow);

        m_tabWidget->addTab(pageWidget, tr("Player %1").arg(player + 1));

        // Wire buttons now that all pages exist (m_pages is reserved, no
        // reallocation, so capturing &page is stable).
        connect(page.setButton, &QPushButton::clicked, this, [&page, this]() {
            const QList<QTableWidgetItem*> selected = page.table->selectedItems();
            if (selected.isEmpty())
            {
                return;
            }
            this->startListening(page, selected.first()->row());
        });
        connect(page.clearButton, &QPushButton::clicked, this, [&page, this]() {
            const QList<QTableWidgetItem*> selected = page.table->selectedItems();
            if (selected.isEmpty())
            {
                return;
            }
            const int row = selected.first()->row();
            auto* nameItem = page.table->item(row, 0);
            if (nameItem == nullptr)
            {
                return;
            }
            const std::string name = nameItem->data(Qt::UserRole).toString().toStdString();
            m_bindings.erase(name);
            this->updateRowDescription(page, row);
        });
        connect(page.deviceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
                [this](int) { /* applied on save */ });
    }
}

void ArcadeInputDialog::switchGame(const QString& driver)
{
    if (driver.isEmpty() || driver == m_driverName)
    {
        return;
    }

    // Persist the current game's bindings before switching to the next one.
    this->saveBindings();

    m_driverName = driver;
    this->loadInputNames();
    this->loadBindings();
    this->buildTabs();
    this->loadSettings();
    this->rebuildTables();
}

void ArcadeInputDialog::loadInputNames(void)
{
    m_inputNames.assign(4, std::vector<std::string>());

    if (!m_pickGameMode)
    {
        for (int player = 0; player < 4; player++)
        {
            const int count = CoreGetFbneoPlayerInputCount(player);
            for (int bit = 0; bit < count; bit++)
            {
                const char* name = CoreGetFbneoPlayerInputName(player, bit);
                if (name != nullptr)
                {
                    m_inputNames[static_cast<size_t>(player)].push_back(name);
                }
            }
        }
        return;
    }

    if (m_driverName.isEmpty())
    {
        return;
    }

    std::vector<std::vector<std::string> > names;
    if (CoreGetFbneoDriverInputNames(m_driverName.toStdString(), names))
    {
        for (size_t p = 0; p < names.size() && p < m_inputNames.size(); p++)
        {
            m_inputNames[p] = std::move(names[p]);
        }
    }
}

void ArcadeInputDialog::loadBindings(void)
{
    m_bindings.clear();
    m_section = "RMG Arcade Input - Global";

    auto readSection = [](const std::string& section) {
        std::unordered_map<std::string, std::vector<Binding>> map;
        const std::vector<std::string> names = CoreSettingsGetStringListValue(SettingsID::FBNeo_BindingNames, section);
        const std::vector<int>         types = CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingTypes, section);
        const std::vector<int>         data  = CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingData, section);
        const std::vector<int>         extra = CoreSettingsGetIntListValue(SettingsID::FBNeo_BindingExtraData, section);

        const size_t count = std::min({ names.size(), types.size(), data.size(), extra.size() });
        for (size_t i = 0; i < count; i++)
        {
            if (names[i].empty())
            {
                continue;
            }
            Binding binding;
            binding.type  = types[i];
            binding.data  = data[i];
            binding.extra = extra[i];
            map[names[i]].push_back(binding);
        }
        return map;
    };

    // Global bindings first, then the per-driver section overlays them.
    m_bindings = readSection("RMG Arcade Input - Global");
    const std::string driver = m_driverName.toStdString();
    if (!driver.empty())
    {
        m_section = "RMG Arcade Input - " + driver;
        for (auto& entry : readSection(m_section))
        {
            m_bindings[entry.first] = std::move(entry.second);
        }
    }
}

void ArcadeInputDialog::saveBindings(void)
{
    std::vector<std::string> names;
    std::vector<int>         types;
    std::vector<int>         data;
    std::vector<int>         extra;

    for (const auto& entry : m_bindings)
    {
        for (const Binding& binding : entry.second)
        {
            names.push_back(entry.first);
            types.push_back(binding.type);
            data.push_back(binding.data);
            extra.push_back(binding.extra);
        }
    }

    CoreSettingsSetValue(SettingsID::FBNeo_BindingNames, m_section, names);
    CoreSettingsSetValue(SettingsID::FBNeo_BindingTypes, m_section, types);
    CoreSettingsSetValue(SettingsID::FBNeo_BindingData, m_section, data);
    CoreSettingsSetValue(SettingsID::FBNeo_BindingExtraData, m_section, extra);
}

void ArcadeInputDialog::loadSettings(void)
{
    for (PlayerPage& page : m_pages)
    {
        const std::string deviceSection = "RMG Arcade Device - P" + std::to_string(page.player);
        const int         deviceType = CoreSettingsGetIntValue(SettingsID::FBNeo_DeviceType, deviceSection);
        const std::string deviceName = CoreSettingsGetStringValue(SettingsID::FBNeo_DeviceName, deviceSection);
        const std::string devicePath = CoreSettingsGetStringValue(SettingsID::FBNeo_DevicePath, deviceSection);
        const int         deviceNum  = CoreSettingsGetIntValue(SettingsID::FBNeo_DeviceNum, deviceSection);

        int index = 0; // default: Automatic
        if (deviceType == 3)
        {
            for (size_t i = 0; i < page.deviceOptions.size(); i++)
            {
                const DeviceOption& option = page.deviceOptions[i];
                if (option.type != 3)
                {
                    continue;
                }
                const bool pathMatch = !devicePath.empty() && option.path == devicePath;
                const bool nameMatch = !deviceName.empty() && option.name == deviceName;
                const bool numMatch  = deviceNum >= 0 && option.num == deviceNum;
                if (pathMatch || nameMatch || numMatch)
                {
                    index = static_cast<int>(i);
                    break;
                }
            }
        }
        else if (deviceType == 2)
        {
            index = 1; // Keyboard
        }

        page.deviceCombo->setCurrentIndex(index);
    }
}

void ArcadeInputDialog::saveSettings(void)
{
    this->saveBindings();

    for (const PlayerPage& page : m_pages)
    {
        const int index = page.deviceCombo->currentIndex();
        if (index < 0 || index >= static_cast<int>(page.deviceOptions.size()))
        {
            continue;
        }
        const DeviceOption& option = page.deviceOptions[static_cast<size_t>(index)];

        const std::string deviceSection = "RMG Arcade Device - P" + std::to_string(page.player);
        CoreSettingsSetValue(SettingsID::FBNeo_DeviceType, deviceSection, option.type);
        CoreSettingsSetValue(SettingsID::FBNeo_DeviceName, deviceSection, option.name);
        CoreSettingsSetValue(SettingsID::FBNeo_DevicePath, deviceSection, option.path);
        CoreSettingsSetValue(SettingsID::FBNeo_DeviceNum, deviceSection, option.num);
    }

    CoreSettingsSave();

    // While editing the controls of a game that is already running, the handler
    // will not see an inactive->active transition again, so reload the mapping
    // live for the new bindings/devices to take effect without a restart.
    if (!m_pickGameMode)
    {
        FbneoInputReloadSettings();
    }
}

void ArcadeInputDialog::rebuildTables(void)
{
    for (PlayerPage& page : m_pages)
    {
        const std::vector<std::string>& names = m_inputNames[static_cast<size_t>(page.player)];
        const int                       count = static_cast<int>(names.size());

        page.table->setRowCount(0);
        page.table->setRowCount(count);
        for (int bit = 0; bit < count; bit++)
        {
            const QString name = QString::fromUtf8(names[static_cast<size_t>(bit)].c_str());
            auto* nameItem = new QTableWidgetItem(name);
            nameItem->setData(Qt::UserRole, name);
            page.table->setItem(bit, 0, nameItem);
            page.table->setItem(bit, 1, new QTableWidgetItem());
            this->updateRowDescription(page, bit);
        }
    }
}

void ArcadeInputDialog::updateRowDescription(PlayerPage& page, int row)
{
    auto* nameItem = page.table->item(row, 0);
    auto* descItem = page.table->item(row, 1);
    if (nameItem == nullptr || descItem == nullptr)
    {
        return;
    }

    const std::string name = nameItem->data(Qt::UserRole).toString().toStdString();
    const auto        it   = m_bindings.find(name);
    if (it == m_bindings.end() || it->second.empty())
    {
        descItem->setText(tr("(unbound)"));
        return;
    }

    descItem->setText(QString::fromStdString(describeBinding(it->second.front())));
}

void ArcadeInputDialog::startListening(PlayerPage& page, int row)
{
    this->stopListening();

    // Use the names enumerated for this driver (buildTabs/rebuildTables built the
    // rows from them). CoreGetFbneoPlayerInputName only works while a session is
    // live, so in pick-a-game mode it would return nothing here.
    if (page.player < 0 || static_cast<size_t>(page.player) >= m_inputNames.size() ||
        row < 0 || static_cast<size_t>(row) >= m_inputNames[static_cast<size_t>(page.player)].size())
    {
        return;
    }

    const std::string name = m_inputNames[static_cast<size_t>(page.player)][static_cast<size_t>(row)];
    if (name.empty())
    {
        return;
    }

    m_listeningPage = &page;
    m_listeningRow  = row;
    m_listeningName = name;
    page.listeningRow = row;

    page.statusLabel->setText(tr("Press a key or button..."));
    m_globalStatus->setText(tr("Press a key or button for \"%1\" (Esc cancels).").arg(QString::fromUtf8(name.c_str())));

    this->grabKeyboard();
    this->openCaptureDevices();
    m_captureTimer->start();
}

void ArcadeInputDialog::stopListening(void)
{
    if (m_captureTimer != nullptr)
    {
        m_captureTimer->stop();
    }
    this->closeCaptureDevices();
    this->releaseKeyboard();

    if (m_listeningPage != nullptr && m_listeningPage->statusLabel != nullptr)
    {
        m_listeningPage->statusLabel->setText(tr("Ready"));
    }
    if (m_listeningPage != nullptr)
    {
        m_listeningPage->listeningRow = -1;
    }

    m_listeningPage = nullptr;
    m_listeningRow  = -1;
    m_listeningName.clear();

    m_globalStatus->setText(tr("Select an input and press \"Set\" to bind a key or button."));
}

void ArcadeInputDialog::commitCapture(const Binding& binding)
{
    if (m_listeningPage == nullptr || m_listeningName.empty())
    {
        return;
    }

    PlayerPage* page   = m_listeningPage;
    const int   row    = m_listeningRow;
    const std::string name = m_listeningName;

    m_bindings[name] = { binding };

    this->stopListening();

    if (row >= 0 && row < page->table->rowCount())
    {
        this->updateRowDescription(*page, row);
        page->table->selectRow(row);
    }
}

void ArcadeInputDialog::openCaptureDevices(void)
{
    this->closeCaptureDevices();

    int               count = 0;
    SDL_JoystickID*   ids   = SDL_GetJoysticks(&count);
    for (int i = 0; i < count; i++)
    {
        CaptureDevice device;
        if (SDL_IsGamepad(ids[i]))
        {
            device.isGamepad = true;
            device.gamepad   = SDL_OpenGamepad(ids[i]);
            if (device.gamepad == nullptr)
            {
                continue;
            }
        }
        else
        {
            device.joystick = SDL_OpenJoystick(ids[i]);
            if (device.joystick == nullptr)
            {
                continue;
            }
        }
        m_captureDevices.push_back(device);
    }
    if (ids != nullptr)
    {
        SDL_free(ids);
    }

    // Seed the previous-state baselines so a control already active when
    // listening started (or an axis resting at an endpoint) is not captured
    // until it changes.
    SDL_PumpEvents();
    SDL_UpdateJoysticks();
    for (CaptureDevice& device : m_captureDevices)
    {
        if (device.isGamepad)
        {
            const int buttons = SDL_GAMEPAD_BUTTON_COUNT;
            const int axes    = SDL_GAMEPAD_AXIS_COUNT;
            device.buttonPrev.assign(static_cast<size_t>(buttons), false);
            device.axisPrev.assign(static_cast<size_t>(axes), 0);
            for (int i = 0; i < buttons; i++)
            {
                device.buttonPrev[static_cast<size_t>(i)] =
                    SDL_GetGamepadButton(device.gamepad, static_cast<SDL_GamepadButton>(i));
            }
            for (int i = 0; i < axes; i++)
            {
                device.axisPrev[static_cast<size_t>(i)] =
                    SDL_GetGamepadAxis(device.gamepad, static_cast<SDL_GamepadAxis>(i));
            }
        }
        else
        {
            const int buttons = SDL_GetNumJoystickButtons(device.joystick);
            const int axes    = SDL_GetNumJoystickAxes(device.joystick);
            const int hats    = SDL_GetNumJoystickHats(device.joystick);
            device.buttonPrev.assign(static_cast<size_t>(buttons), false);
            device.axisPrev.assign(static_cast<size_t>(axes), 0);
            device.hatPrev.assign(static_cast<size_t>(hats), 0);
            for (int i = 0; i < buttons; i++)
            {
                device.buttonPrev[static_cast<size_t>(i)] = SDL_GetJoystickButton(device.joystick, i);
            }
            for (int i = 0; i < axes; i++)
            {
                device.axisPrev[static_cast<size_t>(i)] = SDL_GetJoystickAxis(device.joystick, i);
            }
            for (int i = 0; i < hats; i++)
            {
                device.hatPrev[static_cast<size_t>(i)] = SDL_GetJoystickHat(device.joystick, i);
            }
        }
    }
}

void ArcadeInputDialog::closeCaptureDevices(void)
{
    for (CaptureDevice& device : m_captureDevices)
    {
        if (device.gamepad != nullptr)
        {
            SDL_CloseGamepad(device.gamepad);
            device.gamepad = nullptr;
        }
        else if (device.joystick != nullptr)
        {
            SDL_CloseJoystick(device.joystick);
            device.joystick = nullptr;
        }
    }
    m_captureDevices.clear();
}

bool ArcadeInputDialog::pollCaptureDevices(void)
{
    SDL_PumpEvents();
    SDL_UpdateJoysticks();

    for (CaptureDevice& device : m_captureDevices)
    {
        if (device.isGamepad)
        {
            const int buttons = SDL_GAMEPAD_BUTTON_COUNT;
            for (int i = 0; i < buttons; i++)
            {
                const bool now = SDL_GetGamepadButton(device.gamepad, static_cast<SDL_GamepadButton>(i));
                const bool prev = (static_cast<size_t>(i) < device.buttonPrev.size())
                                      ? device.buttonPrev[static_cast<size_t>(i)]
                                      : false;
                if (now && !prev)
                {
                    m_capturedBinding.type  = Source_GamepadButton;
                    m_capturedBinding.data  = i;
                    m_capturedBinding.extra = 0;
                    return true;
                }
                device.buttonPrev[static_cast<size_t>(i)] = now;
            }

            const int axes = SDL_GAMEPAD_AXIS_COUNT;
            for (int i = 0; i < axes; i++)
            {
                const int now  = SDL_GetGamepadAxis(device.gamepad, static_cast<SDL_GamepadAxis>(i));
                const int prev = (static_cast<size_t>(i) < device.axisPrev.size())
                                     ? device.axisPrev[static_cast<size_t>(i)]
                                     : 0;
                if (std::abs(now) >= (kAxisPeak / 2) && now != prev)
                {
                    m_capturedBinding.type  = Source_GamepadAxis;
                    m_capturedBinding.data  = i;
                    m_capturedBinding.extra = (now > 0) ? 1 : 0;
                    return true;
                }
                device.axisPrev[static_cast<size_t>(i)] = now;
            }
        }
        else
        {
            const int buttons = SDL_GetNumJoystickButtons(device.joystick);
            for (int i = 0; i < buttons; i++)
            {
                const bool now  = SDL_GetJoystickButton(device.joystick, i);
                const bool prev = (static_cast<size_t>(i) < device.buttonPrev.size())
                                      ? device.buttonPrev[static_cast<size_t>(i)]
                                      : false;
                if (now && !prev)
                {
                    m_capturedBinding.type  = Source_JoystickButton;
                    m_capturedBinding.data  = i;
                    m_capturedBinding.extra = 0;
                    return true;
                }
                device.buttonPrev[static_cast<size_t>(i)] = now;
            }

            const int axes = SDL_GetNumJoystickAxes(device.joystick);
            for (int i = 0; i < axes; i++)
            {
                const int now  = SDL_GetJoystickAxis(device.joystick, i);
                const int prev = (static_cast<size_t>(i) < device.axisPrev.size())
                                     ? device.axisPrev[static_cast<size_t>(i)]
                                     : 0;
                if (std::abs(now) >= (kAxisPeak / 2) && now != prev)
                {
                    m_capturedBinding.type  = Source_JoystickAxis;
                    m_capturedBinding.data  = i;
                    m_capturedBinding.extra = (now > 0) ? 1 : 0;
                    return true;
                }
                device.axisPrev[static_cast<size_t>(i)] = now;
            }

            const int hats = SDL_GetNumJoystickHats(device.joystick);
            for (int i = 0; i < hats; i++)
            {
                const int now  = SDL_GetJoystickHat(device.joystick, i);
                const int prev = (static_cast<size_t>(i) < device.hatPrev.size())
                                     ? device.hatPrev[static_cast<size_t>(i)]
                                     : 0;
                if (now != prev && now != 0)
                {
                    m_capturedBinding.type  = Source_JoystickHat;
                    m_capturedBinding.data  = i;
                    m_capturedBinding.extra = now;
                    return true;
                }
                device.hatPrev[static_cast<size_t>(i)] = now;
            }
        }
    }

    return false;
}

void ArcadeInputDialog::keyPressEvent(QKeyEvent* event)
{
    if (m_listeningPage == nullptr || m_listeningName.empty())
    {
        QDialog::keyPressEvent(event);
        return;
    }

    if (event->key() == Qt::Key_Escape)
    {
        this->stopListening();
        return;
    }

    if (event->isAutoRepeat())
    {
        return;
    }

    const int scancode = Utilities::QtKeyToSdl3Key(event->key());
    if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT)
    {
        Binding binding;
        binding.type  = Source_Keyboard;
        binding.data  = scancode;
        binding.extra = 0;
        this->commitCapture(binding);
    }
}

std::string ArcadeInputDialog::describeBinding(const Binding& binding)
{
    switch (binding.type)
    {
        case Source_Keyboard:
        {
            const char* name = SDL_GetScancodeName(static_cast<SDL_Scancode>(binding.data));
            return (name != nullptr && name[0] != '\0') ? std::string("Key ") + name : std::string("Key");
        }
        case Source_GamepadButton:
            return "Button " + std::to_string(binding.data);
        case Source_GamepadAxis:
            return "Axis " + std::to_string(binding.data) + (binding.extra != 0 ? "+" : "-");
        case Source_JoystickButton:
            return "JButton " + std::to_string(binding.data);
        case Source_JoystickAxis:
            return "JAxis " + std::to_string(binding.data) + (binding.extra != 0 ? "+" : "-");
        case Source_JoystickHat:
            return "Hat " + std::to_string(binding.data);
        default:
            return "?";
    }
}
