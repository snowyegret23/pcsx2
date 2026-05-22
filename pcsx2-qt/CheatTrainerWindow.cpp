// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "CheatTrainerWindow.h"

#include "pcsx2/Host.h"
#include "pcsx2/Patch.h"
#include "pcsx2/VMManager.h"

#include <QtCore/QByteArray>
#include <QtGui/QCloseEvent>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <utility>

static constexpr const char* CHEAT_TRAINER_SETTINGS_SECTION = "CheatTrainer/UserInterface";

CheatTrainerWindow::CheatTrainerWindow(QWidget* parent, QString serial, quint32 crc)
	: QDialog(parent)
	, m_serial(std::move(serial))
	, m_crc(crc)
{
	setWindowTitle(tr("Cheat Trainer"));
	resize(900, 520);

	m_enable_cheats = new QCheckBox(tr("Enable Cheats"), this);
	m_reload_button = new QPushButton(tr("Reload"), this);
	m_enable_all_button = new QPushButton(tr("Enable All"), this);
	m_disable_all_button = new QPushButton(tr("Disable All"), this);

	QHBoxLayout* toolbar_layout = new QHBoxLayout();
	toolbar_layout->addWidget(m_enable_cheats);
	toolbar_layout->addStretch();
	toolbar_layout->addWidget(m_reload_button);
	toolbar_layout->addWidget(m_enable_all_button);
	toolbar_layout->addWidget(m_disable_all_button);

	m_cheat_list = new QTreeWidget(this);
	m_cheat_list->setColumnCount(5);
	m_cheat_list->setHeaderLabels({tr("Enabled"), tr("Name"), tr("Applied"), tr("Author"), tr("Description")});
	m_cheat_list->setAlternatingRowColors(true);
	m_cheat_list->setRootIsDecorated(false);
	m_cheat_list->setUniformRowHeights(true);
	m_cheat_list->header()->setStretchLastSection(true);
	m_cheat_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_cheat_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	m_cheat_list->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	m_cheat_list->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	m_cheat_list->header()->setSectionResizeMode(4, QHeaderView::Stretch);

	QVBoxLayout* main_layout = new QVBoxLayout(this);
	main_layout->addLayout(toolbar_layout);
	main_layout->addWidget(m_cheat_list);
	setLayout(main_layout);

	connect(m_reload_button, &QPushButton::clicked, this, &CheatTrainerWindow::reloadList);
	connect(m_enable_all_button, &QPushButton::clicked, this, &CheatTrainerWindow::enableAll);
	connect(m_disable_all_button, &QPushButton::clicked, this, &CheatTrainerWindow::disableAll);
	connect(m_enable_cheats, &QCheckBox::toggled, this, &CheatTrainerWindow::onEnableCheatsToggled);
	connect(m_cheat_list, &QTreeWidget::itemChanged, this, &CheatTrainerWindow::onItemChanged);

	reloadList();
	restoreWindowGeometry();
}

CheatTrainerWindow::~CheatTrainerWindow() = default;

bool CheatTrainerWindow::shouldShowOnStartup()
{
	return Host::GetBaseBoolSettingValue(CHEAT_TRAINER_SETTINGS_SECTION, "ShowOnStartup", false);
}

void CheatTrainerWindow::setShowOnStartup(bool enabled)
{
	Host::SetBaseBoolSettingValue(CHEAT_TRAINER_SETTINGS_SECTION, "ShowOnStartup", enabled);
	Host::CommitBaseSettingChanges();
}

void CheatTrainerWindow::setGame(QString serial, quint32 crc)
{
	if (m_serial == serial && m_crc == crc)
		return;

	m_serial = std::move(serial);
	m_crc = crc;
	reloadList();
}

void CheatTrainerWindow::saveWindowGeometry()
{
	const std::string old_geometry = Host::GetBaseStringSettingValue(CHEAT_TRAINER_SETTINGS_SECTION, "WindowGeometry");
	const std::string geometry = saveGeometry().toBase64().toStdString();
	if (geometry != old_geometry)
	{
		Host::SetBaseStringSettingValue(CHEAT_TRAINER_SETTINGS_SECTION, "WindowGeometry", geometry.c_str());
		Host::CommitBaseSettingChanges();
	}
}

void CheatTrainerWindow::closeEvent(QCloseEvent* event)
{
	saveWindowGeometry();
	QDialog::closeEvent(event);
}

void CheatTrainerWindow::restoreWindowGeometry()
{
	const std::string geometry = Host::GetBaseStringSettingValue(CHEAT_TRAINER_SETTINGS_SECTION, "WindowGeometry");
	if (!geometry.empty())
		restoreGeometry(QByteArray::fromBase64(QByteArray::fromStdString(geometry)));
}

void CheatTrainerWindow::reloadList()
{
	m_updating = true;
	m_cheat_list->clear();
	if (m_serial.isEmpty() || m_crc == 0)
	{
		m_enable_cheats->setChecked(false);
		m_enable_cheats->setEnabled(false);
		m_cheat_list->setEnabled(false);
		m_enable_all_button->setEnabled(false);
		m_disable_all_button->setEnabled(false);
		m_updating = false;
		return;
	}

	std::vector<std::string> enabled_cheats;
	bool cheats_enabled = false;
	Host::RunOnCPUThread([&enabled_cheats, &cheats_enabled]() {
		enabled_cheats = Patch::GetEnabledCheats();
		cheats_enabled = Patch::GetCheatsGloballyEnabled();
	}, true);

	m_enable_cheats->setChecked(cheats_enabled);
	m_enable_cheats->setEnabled(true);
	m_cheat_list->setEnabled(cheats_enabled);
	m_enable_all_button->setEnabled(cheats_enabled);
	m_disable_all_button->setEnabled(cheats_enabled);

	u32 unlabelled_count = 0;
	const std::vector<Patch::PatchInfo> cheats =
		Patch::GetPatchInfo(m_serial.toStdString(), m_crc, true, false, &unlabelled_count);
	for (const Patch::PatchInfo& cheat : cheats)
	{
		QTreeWidgetItem* item = new QTreeWidgetItem(m_cheat_list);
		const bool enabled = std::find(enabled_cheats.begin(), enabled_cheats.end(), cheat.name) != enabled_cheats.end();
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(0, enabled ? Qt::Checked : Qt::Unchecked);
		item->setData(0, Qt::UserRole, QString::fromStdString(cheat.name));
		item->setText(1, QString::fromStdString(cheat.name));
		item->setText(2, QString::fromUtf8(Patch::PlaceToString(cheat.place)));
		item->setText(3, QString::fromStdString(cheat.author));
		item->setText(4, QString::fromStdString(cheat.description));
	}

	if (unlabelled_count > 0)
	{
		QTreeWidgetItem* item = new QTreeWidgetItem(m_cheat_list);
		item->setText(1, tr("%1 unlabelled patch codes will automatically activate.").arg(unlabelled_count));
		item->setFlags(item->flags() & ~Qt::ItemIsUserCheckable);
	}

	m_updating = false;
}

void CheatTrainerWindow::onEnableCheatsToggled(bool enabled)
{
	if (m_updating)
		return;

	Host::RunOnCPUThread([enabled]() {
		Patch::SetCheatsGloballyEnabled(enabled, true, true);
	}, true);

	m_cheat_list->setEnabled(enabled);
	m_enable_all_button->setEnabled(enabled);
	m_disable_all_button->setEnabled(enabled);
}

void CheatTrainerWindow::onItemChanged(QTreeWidgetItem* item, int column)
{
	if (m_updating || column != 0)
		return;

	const QString name = item->data(0, Qt::UserRole).toString();
	if (name.isEmpty())
		return;

	setCheatEnabled(name, item->checkState(0) == Qt::Checked);
}

void CheatTrainerWindow::enableAll()
{
	setAllCheats(true);
}

void CheatTrainerWindow::disableAll()
{
	setAllCheats(false);
}

void CheatTrainerWindow::setAllCheats(bool enabled)
{
	m_updating = true;
	for (int i = 0; i < m_cheat_list->topLevelItemCount(); i++)
	{
		QTreeWidgetItem* item = m_cheat_list->topLevelItem(i);
		const QString name = item->data(0, Qt::UserRole).toString();
		if (name.isEmpty())
			continue;

		item->setCheckState(0, enabled ? Qt::Checked : Qt::Unchecked);
		setCheatEnabled(name, enabled);
	}
	m_updating = false;
}

void CheatTrainerWindow::setCheatEnabled(const QString& name, bool enabled)
{
	bool ok = false;
	const std::string name_string = name.toStdString();
	Host::RunOnCPUThread([&ok, name_string, enabled]() {
		ok = Patch::SetCheatEnabled(name_string, enabled, true, true);
	}, true);

	if (!ok)
		QMessageBox::warning(this, tr("Cheat Trainer"), tr("Failed to change '%1'.").arg(name));
}

#include "moc_CheatTrainerWindow.cpp"
