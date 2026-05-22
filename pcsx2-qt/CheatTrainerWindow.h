// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <QtCore/QString>
#include <QtWidgets/QDialog>

#include <vector>

class QCheckBox;
class QCloseEvent;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

class CheatTrainerWindow final : public QDialog
{
	Q_OBJECT

public:
	CheatTrainerWindow(QWidget* parent, QString serial, quint32 crc);
	~CheatTrainerWindow() override;

	static bool shouldShowOnStartup();
	static void setShowOnStartup(bool enabled);

	void setGame(QString serial, quint32 crc);
	void saveWindowGeometry();

protected:
	void closeEvent(QCloseEvent* event) override;

private Q_SLOTS:
	void reloadList();
	void onEnableCheatsToggled(bool enabled);
	void onItemChanged(QTreeWidgetItem* item, int column);
	void enableAll();
	void disableAll();

private:
	void restoreWindowGeometry();
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
