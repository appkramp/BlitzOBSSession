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
		clearSessions();
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
		subscribeAll();
}

void StatsClient::setExtraAccounts(const QList<AccountRef> &accounts)
{
	QList<AccountRef> wanted;
	for (const AccountRef &a : accounts)
		if (!a.isOwn() && !a.realm.isEmpty() && !wanted.contains(a))
			wanted.append(a);
	if (wanted == extras_)
		return;

	for (const AccountRef &old : std::as_const(extras_)) {
		if (wanted.contains(old))
			continue;
		const QString key = keyOf(old);
		if (key == ownKey())
			continue; // the key's own account, under another name
		feeds_.remove(key);
		if (connected_ && version_ >= 2)
			socket_->sendText(protocol::unsubscribe(old.id, old.realm));
	}
	const QList<AccountRef> before = extras_;
	extras_ = wanted;
	for (const AccountRef &a : std::as_const(extras_)) {
		if (before.contains(a))
			continue;
		feeds_[keyOf(a)];
		if (connected_)
			subscribe(a);
	}
	markDataChanged();
	emit stateChanged();
}

const Feed *StatsClient::feed(const AccountRef &ref) const
{
	auto it = feeds_.constFind(keyOf(ref));
	return it == feeds_.constEnd() ? nullptr : &it.value();
}

QString StatsClient::keyOf(const AccountRef &ref) const
{
	return ref.isOwn() ? ownKey() : keyOf(ref.id, ref.realm);
}

// The key's own account may also be asked for by its id; it is one feed.
QString StatsClient::keyOf(qint64 accountId, const QString &realm) const
{
	if (accountId == 0 || (accountId == account_.accountId && realm == config_.realm))
		return ownKey();
	return QStringLiteral("%1:%2").arg(accountId).arg(realm);
}

void StatsClient::clearSessions()
{
	for (Feed &f : feeds_)
		f.session.clear();
	feeds_[ownKey()];
	markDataChanged();
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
	// Every connection tries the newest protocol first, so a server updated
	// meanwhile is noticed; only the one right after a refusal does not.
	version_ = fallingBack_ ? 1 : protocol::kVersion;
	fallingBack_ = false;
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
	clearSessions();
	if (connected_)
		subscribeAll();
	else if (!config_.key.isEmpty())
		connectNow();
}

void StatsClient::subscribeAll()
{
	subscribe(AccountRef{});
	for (const AccountRef &a : std::as_const(extras_))
		subscribe(a);
	setState(State::Loading);
}

// Resumes where the feed's records end, for the same period: a reconnect
// gets only what it missed.
void StatsClient::subscribe(const AccountRef &ref)
{
	const QString key = keyOf(ref);
	Feed &f = feeds_[key];
	if (!ref.isOwn() && key == ownKey())
		return; // the key's own account, already subscribed
	if (!ref.isOwn() && version_ < 2) {
		f.status = Feed::Status::Unsupported;
		emit stateChanged();
		return;
	}
	f.status = Feed::Status::Loading;
	f.error.clear();
	const QString realm = ref.isOwn() ? config_.realm : ref.realm;
	socket_->sendText(protocol::subscribe(ref.id, realm, periodStart_, f.session.lastId()));
}

void StatsClient::onOpened()
{
	sinceReceived_.restart();
	sincePing_.restart();
	socket_->sendText(protocol::hello(config_.key, clientId(), version_));
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
		maxExtra_ = m.maxExtraAccounts;
		obs_log(LOG_INFO, "connected as %s (%lld), protocol %d", qPrintable(account_.nickname),
			static_cast<long long>(account_.accountId), version_);
		feeds_[ownKey()].nickname = account_.nickname;
		feeds_[ownKey()].clanTag = account_.clanTag;
		subscribeAll();
		break;

	case Type::Account:
		account_ = m.account;
		emit stateChanged();
		break;

	case Type::Subscribed: {
		const QString key = keyOf(m.accountId, m.realm);
		if (!feeds_.contains(key))
			break; // dropped since
		Feed &f = feeds_[key];
		f.nickname = m.nickname;
		f.clanTag = m.clanTag;
		f.hasReading = m.hasReading;
		markDataChanged();
		emit stateChanged();
		break;
	}

	case Type::History:
	case Type::Battles: {
		// Protocol 1 names no account: everything is the key's own.
		const QString key = version_ >= 2 ? keyOf(m.accountId, m.realm) : ownKey();
		if (key == ownKey() && m.realm != config_.realm)
			break; // an answer to a subscription since replaced
		auto it = feeds_.find(key);
		if (it == feeds_.end())
			break;
		bool changed = false;
		for (const BattleRecord &r : m.battles)
			changed = it->session.add(r) || changed;
		if (changed)
			markDataChanged();
		if (m.type == Type::History && !m.hasMore) {
			it->status = Feed::Status::Live;
			if (key == ownKey())
				setState(State::Live);
			emit stateChanged();
		}
		break;
	}

	case Type::Error:
		obs_log(LOG_WARNING, "server error %s: %s", qPrintable(m.errorCode), qPrintable(m.errorMessage));
		if (!m.fatal && (m.accountId != 0 || !m.realm.isEmpty()) && version_ >= 2) {
			onFeedError(m);
			break;
		}
		lastError_ = m.errorMessage.isEmpty() ? m.errorCode : m.errorMessage;
		if (m.errorCode == QLatin1String("key_invalid")) {
			stop();
			setState(State::KeyInvalid, lastError_);
		} else if (m.errorCode == QLatin1String("key_expired")) {
			stop();
			reconnectTimer_.start(kExpiredRetryMs);
			setState(State::KeyExpired, lastError_);
		} else if (m.errorCode == QLatin1String("protocol") && version_ > 1) {
			// A server not yet updated: the key's own account still works.
			obs_log(LOG_INFO, "the server speaks protocol 1: other accounts are not available");
			fallingBack_ = true;
			scheduleReconnect(0);
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

	case Type::Unsubscribed:
	case Type::Pong:
	case Type::Unknown:
		break;
	}
}

// An error about one subscription; the others go on.
void StatsClient::onFeedError(const protocol::Message &m)
{
	const QString key = keyOf(m.accountId, m.realm);
	auto it = feeds_.find(key);
	if (it == feeds_.end())
		return;
	it->error = m.errorMessage.isEmpty() ? m.errorCode : m.errorMessage;
	if (m.errorCode == QLatin1String("account_unknown"))
		it->status = Feed::Status::Unknown;
	else if (m.errorCode == QLatin1String("too_many_accounts"))
		it->status = Feed::Status::TooMany;
	else if (m.errorCode == QLatin1String("realm_unavailable"))
		it->status = Feed::Status::RealmUnavailable;
	else
		it->status = Feed::Status::Failed;

	if (key == ownKey() && it->status == Feed::Status::RealmUnavailable)
		setState(State::RealmUnavailable, it->error);

	// Wargaming could not be reached to check a new account: ask again.
	if (m.retryAfter > 0) {
		const AccountRef ref = key == ownKey() ? AccountRef{} : AccountRef{m.accountId, m.realm};
		QTimer::singleShot(m.retryAfter * 1000, this, [this, ref] {
			if (connected_ && (ref.isOwn() || extras_.contains(ref)))
				subscribe(ref);
		});
	}
	emit stateChanged();
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
