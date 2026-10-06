#include "settings-dialog.h"

#include "plugin.h"
#include "stats-client.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QRadioButton>
#include <QToolButton>
#include <QVBoxLayout>

namespace bss {

namespace {

QPointer<SettingsDialog> g_dialog;

QLabel *hint(const QString &text)
{
	auto *label = new QLabel(text);
	label->setWordWrap(true);
	QFont f = label->font();
	f.setPointSizeF(f.pointSizeF() * 0.9);
	label->setFont(f);
	label->setStyleSheet(QStringLiteral("color: palette(mid);"));
	return label;
}

} // namespace

SettingsDialog::SettingsDialog(QWidget *parent) : QDialog(parent)
{
	setWindowTitle(uiText("Settings.Title"));
	setMinimumWidth(480);

	key_ = new QLineEdit;
	key_->setEchoMode(QLineEdit::Password);
	key_->setClearButtonEnabled(true);
	auto *show = new QToolButton;
	show->setText(uiText("Settings.ShowKey"));
	show->setCheckable(true);
	connect(show, &QToolButton::toggled, this,
		[this](bool on) { key_->setEchoMode(on ? QLineEdit::Normal : QLineEdit::Password); });
	auto *keyRow = new QHBoxLayout;
	keyRow->addWidget(key_, 1);
	keyRow->addWidget(show);
	keyWarning_ = hint(uiText("Settings.KeyFormat"));
	connect(key_, &QLineEdit::textChanged, this, &SettingsDialog::updateKeyHint);

	realm_ = new QComboBox;
	for (const char *realm : {"eu", "com", "asia"})
		realm_->addItem(uiText(QByteArray("Realm.").append(realm).constData()), QString::fromLatin1(realm));

	today_ = new QRadioButton(uiText("Settings.PeriodToday"));
	since_ = new QRadioButton(uiText("Settings.PeriodSince"));
	sinceEdit_ = new QDateTimeEdit;
	sinceEdit_->setCalendarPopup(true);
	sinceEdit_->setDisplayFormat(QStringLiteral("dd.MM.yyyy HH:mm"));
	connect(since_, &QRadioButton::toggled, sinceEdit_, &QWidget::setEnabled);
	auto *sinceRow = new QHBoxLayout;
	sinceRow->addWidget(since_);
	sinceRow->addWidget(sinceEdit_, 1);
	auto *period = new QVBoxLayout;
	period->addWidget(today_);
	period->addLayout(sinceRow);
	period->addWidget(hint(uiText("Settings.PeriodHint")));

	language_ = new QComboBox;
	for (const char *lang : {"auto", "ru", "en", "uk"})
		language_->addItem(uiText(QByteArray("Language.").append(lang).constData()), QString::fromLatin1(lang));

	server_ = new QLineEdit;
	server_->setPlaceholderText(QString::fromLatin1(Config::kDefaultServer));

	status_ = new QLabel;
	status_->setWordWrap(true);
	status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	auto *reload = new QPushButton(uiText("Settings.Reload"));
	reload->setToolTip(uiText("Settings.ReloadHint"));
	connect(reload, &QPushButton::clicked, this, [] {
		if (Plugin *p = plugin())
			p->client->reload();
	});
	auto *statusBox = new QVBoxLayout;
	statusBox->addWidget(status_);
	statusBox->addWidget(reload, 0, Qt::AlignLeft);

	auto *form = new QFormLayout;
	form->addRow(uiText("Settings.Key"), keyRow);
	form->addRow(QString(), keyWarning_);
	form->addRow(QString(), hint(uiText("Settings.KeyHint")));
	form->addRow(uiText("Settings.Realm"), realm_);
	form->addRow(uiText("Settings.Period"), period);
	form->addRow(uiText("Settings.Language"), language_);
	form->addRow(uiText("Settings.Status"), statusBox);

	auto *advanced = new QGroupBox(uiText("Settings.Advanced"));
	advanced->setCheckable(true);
	advanced->setChecked(false);
	auto *advancedForm = new QFormLayout(advanced);
	advancedForm->addRow(uiText("Settings.Server"), server_);
	server_->setVisible(false);
	advancedForm->labelForField(server_)->setVisible(false);
	connect(advanced, &QGroupBox::toggled, this, [this, advancedForm](bool on) {
		server_->setVisible(on);
		advancedForm->labelForField(server_)->setVisible(on);
	});

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Close);
	connect(buttons, &QDialogButtonBox::accepted, this, &SettingsDialog::save);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addLayout(form);
	layout->addWidget(advanced);
	auto *version = new QLabel(uiText("Prop.Version").arg(QString::fromUtf8(PLUGIN_VERSION)));
	version->setStyleSheet(QStringLiteral("color: palette(mid);"));
	auto *footer = new QHBoxLayout;
	footer->addWidget(version);
	footer->addStretch(1);
	footer->addWidget(buttons);
	layout->addLayout(footer);

	if (Plugin *p = plugin()) {
		connect(p->client, &StatsClient::stateChanged, this, &SettingsDialog::updateStatus);
		connect(p->client, &StatsClient::dataChanged, this, &SettingsDialog::updateStatus);
	}
	loadConfig();
}

void SettingsDialog::loadConfig()
{
	Plugin *p = plugin();
	if (!p)
		return;
	const Config &c = p->client->config();
	key_->setText(c.key);
	realm_->setCurrentIndex(std::max(0, realm_->findData(c.realm)));
	today_->setChecked(c.period == Config::Period::Today);
	since_->setChecked(c.period == Config::Period::Since);
	sinceEdit_->setEnabled(c.period == Config::Period::Since);
	sinceEdit_->setDateTime(c.since.isValid() ? c.since.toLocalTime()
						  : QDateTime(QDate::currentDate(), QTime(0, 0)));
	language_->setCurrentIndex(std::max(0, language_->findData(c.language)));
	server_->setText(c.server == QLatin1String(Config::kDefaultServer) ? QString() : c.server);
	updateKeyHint();
	updateStatus();
}

void SettingsDialog::save()
{
	Plugin *p = plugin();
	if (!p)
		return;
	Config c = p->client->config();
	c.key = key_->text().trimmed();
	c.realm = realm_->currentData().toString();
	c.period = since_->isChecked() ? Config::Period::Since : Config::Period::Today;
	c.since = sinceEdit_->dateTime();
	c.language = language_->currentData().toString();
	c.server = server_->text().trimmed().isEmpty() ? QString::fromLatin1(Config::kDefaultServer)
						       : server_->text().trimmed();
	c.save();
	p->client->setConfig(c);
	renderAllSources(); // the language may have changed
	updateStatus();
}

void SettingsDialog::updateKeyHint()
{
	const QString key = key_->text().trimmed();
	keyWarning_->setVisible(!key.isEmpty() && !protocol::looksLikeKey(key));
}

void SettingsDialog::updateStatus()
{
	Plugin *p = plugin();
	if (!p)
		return;
	const StatsClient &client = *p->client;
	QString text;
	switch (client.state()) {
	case StatsClient::State::NoKey:
		text = uiText("Status.NoKey");
		break;
	case StatsClient::State::Connecting:
		text = uiText("Status.Connecting");
		break;
	case StatsClient::State::Loading:
		text = uiText("Status.Loading");
		break;
	case StatsClient::State::Live: {
		QString who = client.account().nickname;
		if (!client.account().clanTag.isEmpty())
			who += QStringLiteral(" [%1]").arg(client.account().clanTag);
		text = uiText("Status.Live").arg(who);
		break;
	}
	case StatsClient::State::Waiting:
		text = uiText("Status.Waiting").arg(client.lastError());
		break;
	case StatsClient::State::RealmUnavailable:
		text = uiText("Status.RealmUnavailable");
		break;
	case StatsClient::State::TooManyConnections:
		text = uiText("Status.TooManyConnections");
		break;
	case StatsClient::State::KeyInvalid:
		text = uiText("Status.KeyInvalid");
		break;
	case StatsClient::State::KeyExpired:
		text = uiText("Status.KeyExpired");
		break;
	case StatsClient::State::Outdated:
		text = uiText("Status.Outdated");
		break;
	}
	if (client.state() == StatsClient::State::Live || client.state() == StatsClient::State::Loading) {
		const int battles = static_cast<int>(client.session().totals().value(QStringLiteral("battles")));
		text += QLatin1Char('\n') +
			uiText("Status.Summary")
				.arg(battles)
				.arg(QLocale().toString(client.periodStart().toLocalTime(), QLocale::ShortFormat));
	}
	// The other accounts the "account" sources show.
	for (const AccountRef &ref : client.extraAccounts()) {
		const Feed *f = client.feed(ref);
		QString what;
		if (client.protocolVersion() < 2)
			what = uiText("Copy.Unsupported");
		else if (!f || f->status == Feed::Status::Waiting || f->status == Feed::Status::Loading)
			what = uiText("Copy.Waiting");
		else if (f->status == Feed::Status::Live)
			what = f->clanTag.isEmpty() ? f->nickname
						    : QStringLiteral("%1 [%2]").arg(f->nickname, f->clanTag);
		else if (f->status == Feed::Status::TooMany)
			what = uiText("Copy.TooMany").arg(client.maxExtraAccounts());
		else if (f->status == Feed::Status::Unknown)
			what = uiText("Copy.Unknown");
		else if (f->status == Feed::Status::RealmUnavailable)
			what = uiText("Copy.RealmUnavailable");
		else
			what = uiText("Copy.Failed").arg(f->error);
		text += QStringLiteral("\n%1 (%2): %3").arg(ref.id).arg(ref.realm.toUpper(), what);
	}
	status_->setText(text);
}

void openSettings()
{
	if (!g_dialog) {
		auto *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
		g_dialog = new SettingsDialog(parent);
		g_dialog->setAttribute(Qt::WA_DeleteOnClose);
	} else {
		g_dialog->loadConfig();
	}
	g_dialog->show();
	g_dialog->raise();
	g_dialog->activateWindow();
}

} // namespace bss
