// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "TimeSaveWindow.h"

#include "QtHost.h"

#include "pcsx2/Config.h"
#include "pcsx2/Host.h"
#include "pcsx2/VMManager.h"

#include "common/Path.h"

#include <QtCore/QDateTime>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTimer>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QGridLayout>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTreeWidget>
#include <QtWidgets/QVBoxLayout>

#include <utility>

TimeSaveWindow::TimeSaveWindow(QWidget* parent, QString serial)
	: QDialog(parent)
	, m_serial(std::move(serial))
{
	setWindowTitle(tr("Time Saves"));
	resize(900, 520);

	m_auto_save = new QCheckBox(tr("Auto Save"), this);
	m_interval = new QSpinBox(this);
	m_interval->setRange(1, 3600);
	m_interval->setValue(10);
	m_interval->setSuffix(tr(" sec"));

	m_keep_count = new QSpinBox(this);
	m_keep_count->setRange(1, 1000);
	m_keep_count->setValue(60);

	m_save_now_button = new QPushButton(tr("Save Now"), this);
	m_load_button = new QPushButton(tr("Load"), this);
	m_save_as_button = new QPushButton(tr("Save As..."), this);
	m_delete_button = new QPushButton(tr("Delete"), this);
	m_refresh_button = new QPushButton(tr("Refresh"), this);

	QGridLayout* controls_layout = new QGridLayout();
	controls_layout->addWidget(m_auto_save, 0, 0);
	controls_layout->addWidget(new QLabel(tr("Interval"), this), 0, 1);
	controls_layout->addWidget(m_interval, 0, 2);
	controls_layout->addWidget(new QLabel(tr("Keep"), this), 0, 3);
	controls_layout->addWidget(m_keep_count, 0, 4);
	controls_layout->setColumnStretch(5, 1);

	QHBoxLayout* buttons_layout = new QHBoxLayout();
	buttons_layout->addWidget(m_save_now_button);
	buttons_layout->addWidget(m_load_button);
	buttons_layout->addWidget(m_save_as_button);
	buttons_layout->addWidget(m_delete_button);
	buttons_layout->addStretch();
	buttons_layout->addWidget(m_refresh_button);

	m_save_list = new QTreeWidget(this);
	m_save_list->setColumnCount(3);
	m_save_list->setHeaderLabels({tr("Name"), tr("Created"), tr("Path")});
	m_save_list->setAlternatingRowColors(true);
	m_save_list->setRootIsDecorated(false);
	m_save_list->setUniformRowHeights(true);
	m_save_list->header()->setStretchLastSection(true);
	m_save_list->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
	m_save_list->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	m_save_list->header()->setSectionResizeMode(2, QHeaderView::Stretch);

	QVBoxLayout* main_layout = new QVBoxLayout(this);
	main_layout->addLayout(controls_layout);
	main_layout->addLayout(buttons_layout);
	main_layout->addWidget(m_save_list);
	setLayout(main_layout);

	m_timer = new QTimer(this);
	m_timer->setInterval(m_interval->value() * 1000);

	connect(m_auto_save, &QCheckBox::toggled, this, &TimeSaveWindow::onAutoSaveToggled);
	connect(m_interval, &QSpinBox::valueChanged, this, &TimeSaveWindow::onIntervalChanged);
	connect(m_save_now_button, &QPushButton::clicked, this, &TimeSaveWindow::captureNow);
	connect(m_load_button, &QPushButton::clicked, this, &TimeSaveWindow::loadSelected);
	connect(m_save_as_button, &QPushButton::clicked, this, &TimeSaveWindow::saveSelectedAs);
	connect(m_delete_button, &QPushButton::clicked, this, &TimeSaveWindow::deleteSelected);
	connect(m_refresh_button, &QPushButton::clicked, this, &TimeSaveWindow::refreshList);
	connect(m_timer, &QTimer::timeout, this, &TimeSaveWindow::captureNow);
	connect(m_save_list, &QTreeWidget::itemSelectionChanged, this, &TimeSaveWindow::updateButtons);
	connect(m_save_list, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem*, int) { loadSelected(); });

	refreshList();
}

TimeSaveWindow::~TimeSaveWindow() = default;

void TimeSaveWindow::setGame(QString serial)
{
	if (m_serial == serial)
		return;

	m_serial = std::move(serial);
	refreshList();
}

void TimeSaveWindow::onAutoSaveToggled(bool enabled)
{
	if (enabled)
		m_timer->start();
	else
		m_timer->stop();
}

void TimeSaveWindow::onIntervalChanged(int value)
{
	m_timer->setInterval(value * 1000);
}

void TimeSaveWindow::captureNow()
{
	if (m_serial.isEmpty())
		return;

	QDir().mkpath(getSaveDirectory());
	const QString path = makeSavePath();
	g_emu_thread->saveState(path);
	Host::RunOnCPUThread([]() { VMManager::WaitForSaveStateFlush(); }, true);

	pruneOldSaves();
	refreshList();
}

void TimeSaveWindow::loadSelected()
{
	const QString path = getSelectedPath();
	if (path.isEmpty())
		return;

	g_emu_thread->loadState(path);
}

void TimeSaveWindow::saveSelectedAs()
{
	const QString path = getSelectedPath();
	if (path.isEmpty())
		return;

	Host::RunOnCPUThread([]() { VMManager::WaitForSaveStateFlush(); }, true);
	const QString default_name = QFileInfo(path).fileName();
	const QString dst = QFileDialog::getSaveFileName(this, tr("Save Time Save As"), default_name, tr("PCSX2 Save State (*.p2s)"));
	if (dst.isEmpty())
		return;

	if (QFile::exists(dst) && !QFile::remove(dst))
	{
		QMessageBox::warning(this, tr("Time Saves"), tr("Failed to replace '%1'.").arg(dst));
		return;
	}

	if (!QFile::copy(path, dst))
		QMessageBox::warning(this, tr("Time Saves"), tr("Failed to copy '%1'.").arg(path));
}

void TimeSaveWindow::deleteSelected()
{
	const QString path = getSelectedPath();
	if (path.isEmpty())
		return;

	if (!QFile::remove(path))
		QMessageBox::warning(this, tr("Time Saves"), tr("Failed to delete '%1'.").arg(path));

	refreshList();
}

void TimeSaveWindow::refreshList()
{
	m_save_list->clear();
	if (m_serial.isEmpty())
	{
		updateButtons();
		return;
	}

	const QDir dir(getSaveDirectory());
	const QFileInfoList files = dir.entryInfoList({m_serial + QStringLiteral("_*.p2s")}, QDir::Files, QDir::Time);
	for (const QFileInfo& file : files)
	{
		QTreeWidgetItem* item = new QTreeWidgetItem(m_save_list);
		item->setText(0, makeDisplayName(file.fileName()));
		item->setText(1, file.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
		item->setText(2, file.absoluteFilePath());
		item->setData(0, Qt::UserRole, file.absoluteFilePath());
	}

	updateButtons();
}

QString TimeSaveWindow::getSaveDirectory() const
{
	return QString::fromStdString(Path::Combine(EmuFolders::Savestates, "TimeSaves"));
}

QString TimeSaveWindow::getSelectedPath() const
{
	const QList<QTreeWidgetItem*> selected = m_save_list->selectedItems();
	return selected.isEmpty() ? QString() : selected.front()->data(0, Qt::UserRole).toString();
}

QString TimeSaveWindow::makeDisplayName(const QString& file_name) const
{
	QString name = file_name;
	if (name.endsWith(QStringLiteral(".p2s"), Qt::CaseInsensitive))
		name.chop(4);
	const QString prefix = m_serial + QStringLiteral("_");
	if (name.startsWith(prefix))
	{
		QString time = name.mid(prefix.size());
		time.replace('-', ':');
		return prefix + time;
	}
	return name;
}

QString TimeSaveWindow::makeSavePath() const
{
	const QString safe_time = QDateTime::currentDateTime().toString(QStringLiteral("HH-mm-ss"));
	return QDir(getSaveDirectory()).filePath(QStringLiteral("%1_%2.p2s").arg(m_serial, safe_time));
}

void TimeSaveWindow::pruneOldSaves()
{
	const QDir dir(getSaveDirectory());
	const QFileInfoList files = dir.entryInfoList({m_serial + QStringLiteral("_*.p2s")}, QDir::Files, QDir::Time);
	for (int i = m_keep_count->value(); i < files.size(); i++)
		QFile::remove(files[i].absoluteFilePath());
}

void TimeSaveWindow::updateButtons()
{
	const bool has_selection = !getSelectedPath().isEmpty();
	m_load_button->setEnabled(has_selection);
	m_save_as_button->setEnabled(has_selection);
	m_delete_button->setEnabled(has_selection);
	m_save_now_button->setEnabled(!m_serial.isEmpty());
	m_auto_save->setEnabled(!m_serial.isEmpty());
}

#include "moc_TimeSaveWindow.cpp"
