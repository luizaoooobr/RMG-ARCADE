/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#include "ArcadeDipDialog.hpp"

#include <RMG-Core/Emulation.hpp>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTableWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QLabel>
#include <QIcon>

using namespace UserInterface::Dialog;

ArcadeDipDialog::ArcadeDipDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowIcon(QIcon(":Resource/RMG.png"));
    setWindowTitle(tr("Arcade DIP Switches"));
    setModal(true);
    setMinimumSize(560, 440);

    const int groupCount = CoreGetFbneoDipGroupCount();
    m_originalSelection.resize(static_cast<size_t>(groupCount));
    for (int group = 0; group < groupCount; group++)
    {
        std::string name;
        int         settingCount = 0;
        int         selected     = 0;
        CoreGetFbneoDipGroup(group, name, settingCount, selected);
        m_originalSelection[static_cast<size_t>(group)] = selected;
    }

    m_readOnly = CoreIsSynchronizedNetplayActive();

    buildUi();
    refreshRows();
    adjustSize();
}

void ArcadeDipDialog::buildUi(void)
{
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* hint = new QLabel(m_readOnly ? tr("DIP switches cannot be changed during a synchronized netplay session.")
                                       : tr("Select a group, then choose a setting to change it."), this);
    hint->setWordWrap(true);
    root->addWidget(hint);

    m_table = new QTableWidget(this);
    m_table->setColumnCount(2);
    m_table->setHorizontalHeaderLabels({ tr("DIPSwitch group"), tr("Setting") });
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_table->verticalHeader()->setVisible(false);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setAlternatingRowColors(true);
    root->addWidget(m_table, 1);

    auto* editRow = new QHBoxLayout();
    auto* editLabel = new QLabel(tr("Setting:"), this);
    editRow->addWidget(editLabel);

    m_settingCombo = new QComboBox(this);
    m_settingCombo->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    editRow->addWidget(m_settingCombo, 1);

    auto* defaultsButton = new QPushButton(tr("Defaults"), this);
    editRow->addWidget(defaultsButton);
    root->addLayout(editRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ArcadeDipDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &ArcadeDipDialog::reject);

    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        const QList<QTableWidgetItem*> selected = m_table->selectedItems();
        if (selected.isEmpty())
        {
            return;
        }
        this->selectGroup(selected.first()->row());
    });

    connect(m_settingCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int index) {
        if (m_applying || m_readOnly || m_currentGroup < 0 || index < 0)
        {
            return;
        }
        this->applySetting(index);
    });

    connect(defaultsButton, &QPushButton::clicked, this, [this]() {
        if (m_readOnly)
        {
            return;
        }
        CoreResetFbneoDips();
        // CoreResetFbneoDips clears persisted selections; re-snapshot so Cancel
        // restores what the defaults produced rather than the pre-dialog values.
        const int groupCount = CoreGetFbneoDipGroupCount();
        m_originalSelection.resize(static_cast<size_t>(groupCount));
        for (int group = 0; group < groupCount; group++)
        {
            std::string name;
            int         settingCount = 0;
            int         selected     = 0;
            CoreGetFbneoDipGroup(group, name, settingCount, selected);
            m_originalSelection[static_cast<size_t>(group)] = selected;
        }
        this->refreshRows();
    });

    m_settingCombo->setEnabled(!m_readOnly);
    defaultsButton->setEnabled(!m_readOnly);
}

void ArcadeDipDialog::refreshRows(void)
{
    const int groupCount = CoreGetFbneoDipGroupCount();

    m_applying = true;
    m_table->setRowCount(0);
    m_table->setRowCount(groupCount);

    int firstRow = -1;
    for (int group = 0; group < groupCount; group++)
    {
        std::string name;
        int         settingCount = 0;
        int         selected     = 0;
        if (!CoreGetFbneoDipGroup(group, name, settingCount, selected))
        {
            continue;
        }

        std::string settingName;
        if (selected >= 0 && CoreGetFbneoDipSettingName(group, selected, settingName))
        {
            // keep the resolved setting name
        }
        else
        {
            settingName = tr("(none)").toStdString();
        }

        auto* groupItem = new QTableWidgetItem(QString::fromStdString(name));
        groupItem->setData(Qt::UserRole, group);
        m_table->setItem(group, 0, groupItem);
        m_table->setItem(group, 1, new QTableWidgetItem(QString::fromStdString(settingName)));

        if (firstRow < 0)
        {
            firstRow = group;
        }
    }
    m_applying = false;

    if (firstRow >= 0)
    {
        m_table->selectRow(firstRow);
    }
    else
    {
        m_currentGroup = -1;
        m_settingCombo->clear();
    }
}

void ArcadeDipDialog::selectGroup(int row)
{
    if (row < 0 || row >= m_table->rowCount())
    {
        return;
    }

    std::string name;
    int         settingCount = 0;
    int         selected     = 0;
    if (!CoreGetFbneoDipGroup(row, name, settingCount, selected))
    {
        return;
    }

    m_currentGroup = row;

    m_applying = true;
    m_settingCombo->clear();
    for (int setting = 0; setting < settingCount; setting++)
    {
        std::string settingName;
        if (CoreGetFbneoDipSettingName(row, setting, settingName))
        {
            m_settingCombo->addItem(QString::fromStdString(settingName));
        }
    }
    if (selected >= 0 && selected < m_settingCombo->count())
    {
        m_settingCombo->setCurrentIndex(selected);
    }
    m_applying = false;
}

void ArcadeDipDialog::applySetting(int setting)
{
    if (m_currentGroup < 0 || setting < 0)
    {
        return;
    }

    if (!CoreSetFbneoDip(m_currentGroup, setting))
    {
        return;
    }

    std::string settingName;
    if (CoreGetFbneoDipSettingName(m_currentGroup, setting, settingName))
    {
        m_applying = true;
        if (m_table->item(m_currentGroup, 1) != nullptr)
        {
            m_table->item(m_currentGroup, 1)->setText(QString::fromStdString(settingName));
        }
        m_applying = false;
    }
}

void ArcadeDipDialog::restoreOriginalSelection(void)
{
    const int groupCount = static_cast<int>(m_originalSelection.size());
    for (int group = 0; group < groupCount; group++)
    {
        CoreSetFbneoDip(group, m_originalSelection[static_cast<size_t>(group)]);
    }
}

void ArcadeDipDialog::accept(void)
{
    QDialog::accept();
}

void ArcadeDipDialog::reject(void)
{
    if (!m_readOnly)
    {
        this->restoreOriginalSelection();
    }
    QDialog::reject();
}
