// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include <QtCore/QString>
#include <QtWidgets/QDialog>

class QCheckBox;
class QPushButton;
class QSpinBox;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

class TimeSaveWindow final : public QDialog
{
	Q_OBJECT

public:
	TimeSaveWindow(QWidget* parent, QString serial);
	~TimeSaveWindow() override;

	void setGame(QString serial);

private Q_SLOTS:
	void onAutoSaveToggled(bool enabled);
	void onIntervalChanged(int value);
	void captureNow();
	void loadSelected();
	void saveSelectedAs();
	void deleteSelected();
	void refreshList();

private:
	QString getSaveDirectory() const;
	QString getSelectedPath() const;
	QString makeDisplayName(const QString& file_name) const;
	QString makeSavePath() const;
	void pruneOldSaves();
	void updateButtons();

	QCheckBox* m_auto_save = nullptr;
	QSpinBox* m_interval = nullptr;
	QSpinBox* m_keep_count = nullptr;
	QPushButton* m_save_now_button = nullptr;
	QPushButton* m_load_button = nullptr;
	QPushButton* m_save_as_button = nullptr;
	QPushButton* m_delete_button = nullptr;
	QPushButton* m_refresh_button = nullptr;
	QTreeWidget* m_save_list = nullptr;
	QTimer* m_timer = nullptr;

	QString m_serial;
};
