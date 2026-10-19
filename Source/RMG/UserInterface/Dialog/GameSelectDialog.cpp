/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "GameSelectDialog.hpp"

#include <QDialogButtonBox>
#include <QFileDialog>
#include <QPixmap>
#include <QFileInfo>
#include <QFile>
#include <QDir>
#include <algorithm>

using namespace UserInterface::Dialog;

GameSelectDialog::GameSelectDialog(QWidget *parent, QList<RomBrowserModelData> modelData) : QDialog(parent)
{
    this->setupUi(this);

    this->modelData = modelData;

    // configure search debounce timer
    this->searchDebounceTimer = new QTimer(this);
    this->searchDebounceTimer->setSingleShot(true);
    this->searchDebounceTimer->setInterval(500);
    connect(this->searchDebounceTimer, &QTimer::timeout, this, &GameSelectDialog::on_searchDebounceTimer_timeout);

    // connect filter combo explicitly (auto-connect is unreliable for
    // QComboBox because currentIndexChanged is overloaded)
    connect(this->filterComboBox, &QComboBox::currentIndexChanged, this, &GameSelectDialog::onFilterComboBoxIndexChanged);

    // configure button box
    this->buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Play"));
    this->buttonBox->button(QDialogButtonBox::Ok)->setDefault(true);
    this->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);

    // configure preview label
    this->previewLabel->setScaledContents(false);

    // build the game tree
    this->buildTree();

    // focus the search box
    this->searchLineEdit->setFocus();
}

GameSelectDialog::~GameSelectDialog(void)
{
}

QString GameSelectDialog::getSelectedFile(void)
{
    return this->selectedFile;
}

QString GameSelectDialog::getDisplayName(const RomBrowserModelData& data)
{
    QString name = QString::fromStdString(data.settings.GoodName);
    if (name.endsWith("(unknown rom)") || name.endsWith("(unknown disk)"))
    {
        name = QFileInfo(data.file).fileName();
    }
    return name;
}

QString GameSelectDialog::getSaveTypeName(uint16_t saveType)
{
    QStringList saveTypeNames =
    {
        tr("4 KB EEPROM"),
        tr("16 KB EEPROM"),
        tr("SRAM"),
        tr("Flash RAM"),
        tr("Controller pak"),
        tr("None"),
    };

    if (saveType < (uint16_t)saveTypeNames.size())
    {
        return saveTypeNames[saveType];
    }

    return QString::number(saveType);
}

void GameSelectDialog::buildTree(void)
{
    QString search = this->searchLineEdit->text().trimmed().toLower();
    int filterIndex = this->filterComboBox->currentIndex();

    this->gameTreeWidget->clear();

    // group filtered games by display name
    QMap<QString, QList<RomBrowserModelData>> groups;
    int gameCount = 0;

    for (const RomBrowserModelData& data : this->modelData)
    {
        // type filter
        if ((filterIndex == 1) && (data.type != CoreRomType::Cartridge))
        {
            continue;
        }
        else if ((filterIndex == 2) && (data.type != CoreRomType::Arcade))
        {
            continue;
        }

        // search filter
        QString name = this->getDisplayName(data);
        QString fileName = QFileInfo(data.file).fileName();
        if (!search.isEmpty() &&
            !name.toLower().contains(search) &&
            !fileName.toLower().contains(search))
        {
            continue;
        }

        groups[name].append(data);
        gameCount++;
    }

    // sort group names alphabetically (case insensitive)
    QStringList groupNames = groups.keys();
    groupNames.sort(Qt::CaseInsensitive);

    for (const QString& groupName : groupNames)
    {
        QList<RomBrowserModelData> entries = groups[groupName];

        if (entries.size() == 1)
        {
            // single file: the item itself is the game
            const RomBrowserModelData& data = entries.first();
            QTreeWidgetItem *item = new QTreeWidgetItem(this->gameTreeWidget);
            item->setText(0, groupName);
            item->setData(0, Qt::UserRole, data.file);
            item->setToolTip(0, QDir::toNativeSeparators(data.file));
        }
        else
        {
            // multiple files with the same name: parent + children
            QTreeWidgetItem *parent = new QTreeWidgetItem(this->gameTreeWidget);
            parent->setText(0, groupName);
            parent->setToolTip(0, tr("%1 files").arg(entries.size()));

            // sort children by file name
            std::sort(entries.begin(), entries.end(),
                [](const RomBrowserModelData& a, const RomBrowserModelData& b)
                {
                    return QFileInfo(a.file).fileName() < QFileInfo(b.file).fileName();
                });

            for (const RomBrowserModelData& data : entries)
            {
                QTreeWidgetItem *child = new QTreeWidgetItem(parent);
                child->setText(0, QFileInfo(data.file).fileName());
                child->setData(0, Qt::UserRole, data.file);
                child->setToolTip(0, QDir::toNativeSeparators(data.file));
            }

            parent->setExpanded(true);
        }
    }

    // count label
    this->countLabel->setText(gameCount == 1 ? tr("1 game") : tr("%1 games").arg(gameCount));

    // empty state hint
    if (gameCount == 0)
    {
        QTreeWidgetItem *hint = new QTreeWidgetItem(this->gameTreeWidget);
        if (this->modelData.isEmpty())
        {
            hint->setText(0, tr("No games found. Use Browse... or configure ROM directories in Settings."));
        }
        else
        {
            hint->setText(0, tr("No games match the search."));
        }
        hint->setDisabled(true);
        this->gameTreeWidget->addTopLevelItem(hint);
    }

    // select the first game
    if (this->gameTreeWidget->topLevelItemCount() > 0)
    {
        QTreeWidgetItem *firstItem = this->gameTreeWidget->topLevelItem(0);
        if (firstItem->data(0, Qt::UserRole).toString().isEmpty() && firstItem->childCount() > 0)
        {
            firstItem = firstItem->child(0);
        }
        this->gameTreeWidget->setCurrentItem(firstItem);
    }
    else
    {
        this->updateInfoPanel();
    }
}

void GameSelectDialog::updateInfoPanel(void)
{
    QTreeWidgetItem *item = this->gameTreeWidget->currentItem();
    QString file = item ? item->data(0, Qt::UserRole).toString() : QString();

    bool found = false;
    RomBrowserModelData data;
    for (const RomBrowserModelData& currentData : this->modelData)
    {
        if (currentData.file == file)
        {
            data = currentData;
            found = true;
            break;
        }
    }

    if (!found)
    {
        this->nameLabel->setText("-");
        this->regionLabel->setText("-");
        this->typeLabel->setText("-");
        this->saveLabel->setText("-");
        this->md5Label->setText("-");
        this->fileLabel->setText("-");

        // fallback cover
        QPixmap pixmap(":Resource/CoverFallback.png");
        this->previewLabel->setPixmap(pixmap.scaled(this->previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

        this->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }

    QFileInfo fileInfo(data.file);

    this->nameLabel->setText(this->getDisplayName(data));
    this->regionLabel->setText(QString::fromStdString(data.header.Region).isEmpty() ? "-" : QString::fromStdString(data.header.Region));
    this->typeLabel->setText(data.type == CoreRomType::Arcade ? tr("Arcade") : tr("Cartridge"));
    this->saveLabel->setText(this->getSaveTypeName(data.settings.SaveType));
    this->md5Label->setText(QString::fromStdString(data.settings.MD5));
    this->fileLabel->setText(QDir::toNativeSeparators(data.file));

    // cover preview
    QPixmap pixmap;
    if (!data.coverFile.isEmpty() && QFile::exists(data.coverFile))
    {
        pixmap.load(data.coverFile);
    }
    if (pixmap.isNull())
    {
        pixmap.load(":Resource/CoverFallback.png");
    }
    this->previewLabel->setPixmap(pixmap.scaled(this->previewLabel->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));

    this->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
}

void GameSelectDialog::on_searchLineEdit_textChanged(const QString& text)
{
    Q_UNUSED(text);
    this->searchDebounceTimer->start();
}

void GameSelectDialog::on_searchDebounceTimer_timeout(void)
{
    this->buildTree();
}

void GameSelectDialog::onFilterComboBoxIndexChanged(int index)
{
    Q_UNUSED(index);
    this->buildTree();
}

void GameSelectDialog::on_gameTreeWidget_itemSelectionChanged(void)
{
    this->updateInfoPanel();
}

void GameSelectDialog::on_gameTreeWidget_itemDoubleClicked(QTreeWidgetItem *item, int column)
{
    Q_UNUSED(column);

    QString file = item ? item->data(0, Qt::UserRole).toString() : QString();
    if (!file.isEmpty())
    {
        this->selectedFile = file;
        this->accept();
    }
}

void GameSelectDialog::on_browseButton_clicked(void)
{
    QString romFile = QFileDialog::getOpenFileName(
        this,
        tr("Open N64 ROM or 64DD Disk"),
        "",
        tr("N64 ROMs & Disks (*.n64 *.z64 *.v64 *.ndd *.d64 *.zip *.7z)"));

    if (!romFile.isEmpty())
    {
        this->selectedFile = romFile;
        this->accept();
    }
}

void GameSelectDialog::on_buttonBox_accepted(void)
{
    QTreeWidgetItem *item = this->gameTreeWidget->currentItem();
    QString file = item ? item->data(0, Qt::UserRole).toString() : QString();

    if (file.isEmpty())
    {
        return;
    }

    this->selectedFile = file;
    this->accept();
}
