/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef GAMESELECTDIALOG_HPP
#define GAMESELECTDIALOG_HPP

#include "UserInterface/Widget/RomBrowser/RomBrowserWidget.hpp"

#include <QList>
#include <QString>
#include <QDialog>
#include <QTimer>

#include "ui_GameSelectDialog.h"

namespace UserInterface
{
namespace Dialog
{
class GameSelectDialog : public QDialog, private Ui::GameSelectDialog
{
    Q_OBJECT

  public:
    GameSelectDialog(QWidget *parent, QList<RomBrowserModelData> modelData);
    ~GameSelectDialog(void);

    QString getSelectedFile(void);

  private:
    QList<RomBrowserModelData> modelData;
    QString selectedFile;
    QTimer *searchDebounceTimer = nullptr;

    void buildTree(void);
    void updateInfoPanel(void);
    QString getDisplayName(const RomBrowserModelData& data);
    QString getSaveTypeName(uint16_t saveType);

  private slots:
    void on_searchLineEdit_textChanged(const QString& text);
    void on_searchDebounceTimer_timeout(void);
    void onFilterComboBoxIndexChanged(int index);
    void on_gameTreeWidget_itemSelectionChanged(void);
    void on_gameTreeWidget_itemDoubleClicked(QTreeWidgetItem *item, int column);
    void on_browseButton_clicked(void);
    void on_buttonBox_accepted(void);
};
} // namespace Dialog
} // namespace UserInterface

#endif // GAMESELECTDIALOG_HPP
