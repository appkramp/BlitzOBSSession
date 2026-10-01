#include "stats-client.h"

#include "net/websocket.h"

// obs-module.h first: it declares blogva as imported from libobs, which MSVC
// will not accept after plugin-support.h's plain declaration.
#include <obs-module.h>
#include <plugin-support.h>

#include <QRandomGenerator>
#include <QUrl>

namespace bss {

namespace {

constexpr int kTickMs = 5000;
constexpr qint64 kPingEveryMs = 25000;
constexpr qint64 kSilenceMs = 60000;
constexpr qint64 kConnectTimeoutMs = 30000;
constexpr int kExpiredRetryMs = 60 * 60 * 1000;

QString clientId()
{
#if defined(_WIN32)
	const char *os = "windows";
#elif defined(__APPLE__)
	const char *os = "macos";
#else
	const char *os = "linux";
#endif
	return QStringLiteral("%1/%2 (%3; obs %4)")
		.arg(QString::fromUtf8(PLUGIN_NAME), QString::fromUtf8(PLUGIN_VERSION), QString::fromLatin1(os),
		     QString::fromUtf8(obs_get_version_string()));
}

} // namespace

StatsClient::StatsClient(QObject *parent) : QObject(parent), socket_(WebSocket::create(this))
{
	connect(socket_, &WebSocket::opened, this, &StatsClient::onOpened);
	connect(socket_, &WebSocket::textReceived, this, &StatsClient::onText);
	connect(socket_, &WebSocket::closed, this, &StatsClient::onClosed);

	reconnectTimer_.setSingleShot(true);
	connect(&reconnectTimer_, &QTimer::timeout, this, &StatsClient::connectNow);

	tickTimer_.setInterval(kTickMs);
	connect(&tickTimer_, &QTimer::timeout, this, &StatsClient::onTick);

	dataTimer_.setSingleShot(true);
	dataTimer_.setInterval(50);
	connect(&dataTimer_, &QTimer::timeout, this, &StatsClient::dataChanged);

	sinceReceived_.start();
	sincePing_.start();
}

StatsClient::~StatsClient()
{
	stop();
}

void StatsClient::setConfig(const Config &config)
{
	const bool first = !periodStart_.isValid();
	const bool newConnection = first || !config_.sameConnection(config);
	const bool newSubscription = first || !config_.sameSubscription(config);
	config_ = config;

	if (newConnection || newSubscription) {
		periodStart_ = computePeriodStart();
		session_.clear();
		markDataChanged();
	}

	if (config_.key.isEmpty()) {
		stop();
		setState(State::NoKey);
		return;
	}

	const bool halted = state_ == State::KeyInvalid || state_ == State::KeyExpired || state_ == State::Outdated ||
			    state_ == State::NoKey;
	if (newConnection || halted || stopped_)
		connectNow();
	else if (newSubscription && connected_)
		subscribe();
}

void StatsClient::stop()
{
	stopped_ = true;
	connected_ = false;
	reconnectTimer_.stop();
	tickTimer_.stop();
	socket_->close();
}

QDateTime StatsClient::computePeriodStart() const
{
	if (config_.period == Config::Period::Since && config_.since.isValid())
		return config_.since;
	// "Today" is fixed when the session starts and does not move at midnight:
	// a stream that crosses midnight stays one session.
	return protocol::startOfLocalDay(QDateTime::currentDateTime());
}

void StatsClient::connectNow()
{
	stopped_ = false;
	connected_ = false;
	reconnectTimer_.stop();
	socket_->open(QUrl(config_.server));
	sinceReceived_.restart();
	tickTimer_.start();
	setState(State::Connecting);
}

void StatsClient::scheduleReconnect(int delayMs, State state)
{
	connected_ = false;
	socket_->close();
	reconnectTimer_.start(delayMs);
	setState(state, lastError_);
	obs_log(LOG_INFO, "reconnecting in %d s: %s", delayMs / 1000, qPrintable(lastError_));
}

void StatsClient::reload()
{
	session_.clear();
	markDataChanged();
	if (connected_)
		subscribe();
	else if (!config_.key.isEmpty())
		connectNow();
}

void StatsClient::subscribe()
{
	socket_->sendText(protocol::subscribe(config_.realm, periodStart_, session_.lastId()));
	setState(State::Loading);
}

void StatsClient::onOpened()
{
	sinceReceived_.restart();
	sincePing_.restart();
	socket_->sendText(protocol::hello(config_.key, clientId()));
}

void StatsClient::onText(const QByteArray &text)
{
	sinceReceived_.restart();

	protocol::Message m;
	if (!protocol::parse(text, m)) {
		obs_log(LOG_WARNING, "unreadable message from the server");
		return;
	}

	using Type = protocol::Message::Type;
	switch (m.type) {
	case Type::Welcome:
		connected_ = true;
		attempt_ = 0;
		lastError_.clear();
		account_ = m.account;
		obs_log(LOG_INFO, "connected as %s (%lld)", qPrintable(account_.nickname),
			static_cast<long long>(account_.accountId));
		subscribe();
		break;

	case Type::Account:
		account_ = m.account;
		emit stateChanged();
		break;

	case Type::History:
	case Type::Battles: {
		if (m.realm != config_.realm)
			break; // an answer to a subscription since replaced
		bool changed = false;
		for (const BattleRecord &r : m.battles)
			changed = session_.add(r) || changed;
		if (changed)
			markDataChanged();
		if (m.type == Type::History && !m.hasMore)
			setState(State::Live);
		break;
	}

	case Type::Error:
		obs_log(LOG_WARNING, "server error %s: %s", qPrintable(m.errorCode), qPrintable(m.errorMessage));
		lastError_ = m.errorMessage.isEmpty() ? m.errorCode : m.errorMessage;
		if (m.errorCode == QLatin1String("key_invalid")) {
			stop();
			setState(State::KeyInvalid, lastError_);
		} else if (m.errorCode == QLatin1String("key_expired")) {
			stop();
			reconnectTimer_.start(kExpiredRetryMs);
			setState(State::KeyExpired, lastError_);
		} else if (m.errorCode == QLatin1String("protocol")) {
			stop();
			setState(State::Outdated, lastError_);
		} else if (m.errorCode == QLatin1String("rate_limited")) {
			// The key is already streaming to as many places as it may. Not a
			// fault of the connection, so no backoff growth: the server says
			// when to try again.
			scheduleReconnect(std::max(m.retryAfter, 5) * 1000, State::TooManyConnections);
		} else if (m.errorCode == QLatin1String("realm_unavailable")) {
			setState(State::RealmUnavailable, lastError_);
		} else if (m.fatal) {
			const int backoff =
				protocol::reconnectDelayMs(attempt_++, QRandomGenerator::global()->generateDouble());
			scheduleReconnect(std::max(backoff, m.retryAfter * 1000));
		}
		break;

	case Type::Pong:
	case Type::Unknown:
		break;
	}
}

void StatsClient::onClosed(const QString &reason)
{
	connected_ = false;
	if (stopped_)
		return;
	lastError_ = reason;
	scheduleReconnect(protocol::reconnectDelayMs(attempt_++, QRandomGenerator::global()->generateDouble()));
}

void StatsClient::onTick()
{
	if (stopped_ || reconnectTimer_.isActive())
		return;
	if (connected_) {
		if (sincePing_.elapsed() >= kPingEveryMs) {
			sincePing_.restart();
			socket_->sendText(protocol::ping());
		}
		if (sinceReceived_.elapsed() >= kSilenceMs)
			onClosed(QStringLiteral("no answer for 60 s"));
	} else if (state_ == State::Connecting && sinceReceived_.elapsed() >= kConnectTimeoutMs) {
		onClosed(QStringLiteral("connection timed out"));
	}
}

void StatsClient::setState(State s, const QString &error)
{
	if (!error.isEmpty())
		lastError_ = error;
	if (s == state_ && error.isEmpty())
		return;
	state_ = s;
	emit stateChanged();
}

void StatsClient::markDataChanged()
{
	if (!dataTimer_.isActive())
		dataTimer_.start();
}

} // namespace bss
