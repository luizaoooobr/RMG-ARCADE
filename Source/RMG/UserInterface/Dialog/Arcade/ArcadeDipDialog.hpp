/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 * Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3.
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#ifndef ARCADEDIPDIALOG_HPP
#define ARCADEDIPDIALOG_HPP

#include <QDialog>
#include <string>
#include <vector>

class QTableWidget;
class QComboBox;
class QTableWidgetItem;

namespace UserInterface
{
namespace Dialog
{
// Edits the DIP switches of the running arcade (FBNeo) driver, mirroring the
// native FBNeo "Edit DIPswitch settings" dialog: one row per DIP group showing
// its currently selected setting, with a combo to change the selection and a
// Defaults button to restore the driver's defaults.
class ArcadeDipDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ArcadeDipDialog(QWidget* parent = nullptr);
    ~ArcadeDipDialog() override = default;

protected:
    void accept(void) override;
    void reject(void) override;

private:
    void buildUi(void);
    void refreshRows(void);
    void selectGroup(int row);
    void applySetting(int setting);
    void restoreOriginalSelection(void);

    QTableWidget* m_table        = nullptr;
    QComboBox*    m_settingCombo = nullptr;

    // Original per-group selection captured at open, restored on Cancel.
    std::vector<int> m_originalSelection;
    int              m_currentGroup = -1;
    bool             m_applying     = false;
    bool             m_readOnly     = false;
};
} // namespace Dialog
} // namespace UserInterface

#endif // ARCADEDIPDIALOG_HPP
