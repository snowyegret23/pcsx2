// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <QtCore/QString>
#include <QtWidgets/QDialog>

#include <vector>

class QCheckBox;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class CheatTrainerWindow final : public QDialog
{
	Q_OBJECT

public:
	CheatTrainerWindow(QWidget* parent, QString serial, quint32 crc);
	~CheatTrainerWindow() override;

	void setGame(QString serial, quint32 crc);

private Q_SLOTS:
	void reloadList();
	void onEnableCheatsToggled(bool enabled);
	void onItemChanged(QTreeWidgetItem* item, int column);
	void enableAll();
	void disableAll();

private:
	void setAllCheats(bool enabled);
	void setCheatEnabled(const QString& name, bool enabled);

	QCheckBox* m_enable_cheats = nullptr;
	QTreeWidget* m_cheat_list = nullptr;
	QPushButton* m_reload_button = nullptr;
	QPushButton* m_enable_all_button = nullptr;
	QPushButton* m_disable_all_button = nullptr;

	QString m_serial;
	quint32 m_crc = 0;
	bool m_updating = false;
};
