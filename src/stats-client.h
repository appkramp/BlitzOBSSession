#pragma once

// The connection to the statistics server (docs/overlay-protocol.md) and the
// sessions it fills: the key's own account, and every other account an
// "account" source shows (protocol 2). One per OBS instance, on the Qt main
// thread; every source reads from it.

#include "config.h"
#include "core/protocol.h"
#include "core/session.h"

#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTimer>

namespace bss {

class WebSocket;

// An account a source shows: the key's own (id 0, on the configured realm)
// or another one by its Wargaming id and realm.
struct AccountRef {
	qint64 id = 0;
	QString realm;

	bool isOwn() const { return id == 0; }
	bool operator==(const AccountRef &o) const { return id == o.id && realm == o.realm; }
};

// One subscription and what it has brought.
struct Feed {
	enum class Status {
		Waiting,          // not subscribed yet (no connection, or the server is older)
		Loading,          // subscribed, history arriving
		Live,             // history complete
		Unknown,          // Wargaming does not know the account there
		TooMany,          // over the key's limit of other accounts
		RealmUnavailable, // the realm is not collected (or not the key's, for its own account)
		Failed,           // another error; retried when the server says
		Unsupported,      // the server speaks protocol 1: only the key's own account
	};

	Status status = Status::Waiting;
	Session session;
	QString nickname;
	QString clanTag;
	bool hasReading = true; // false: the server only began to collect it
	QString error;
};

class StatsClient : public QObject {
	Q_OBJECT

public:
	enum class State {
		NoKey,              // nothing to connect with
		Connecting,         // socket opening or hello sent
		Loading,            // subscribed, history arriving
		Live,               // history complete, new battles pushed
		Waiting,            // disconnected, reconnecting after a delay
		RealmUnavailable,   // connected, but the key does not open this realm
		TooManyConnections, // the key is in use elsewhere up to its limit; retrying
		KeyInvalid,         // stopped until the key is changed
		KeyExpired,         // stopped; tried again hourly
		Outdated,           // the server wants a newer plugin
	};

	explicit StatsClient(QObject *parent = nullptr);
	~StatsClient() override;

	void setConfig(const Config &config);
	const Config &config() const { return config_; }
	void stop();
	// Drops what is held and loads the period again from the start — for when
	// the server has since corrected records it already sent (it never takes
	// a record back on its own).
	void reload();

	// The other accounts the sources show; subscribed and unsubscribed to
	// match. The key's own account is always subscribed.
	void setExtraAccounts(const QList<AccountRef> &accounts);
	const QList<AccountRef> &extraAccounts() const { return extras_; }

	State state() const { return state_; }
	QString lastError() const { return lastError_; }
	const protocol::Account &account() const { return account_; }
	QDateTime periodStart() const { return periodStart_; }
	int protocolVersion() const { return version_; }
	int maxExtraAccounts() const { return maxExtra_; }

	// The key's own account, or another; null for one not asked for.
	const Feed *feed(const AccountRef &ref) const;
	const Session &session() const { return feeds_[ownKey()].session; }

signals:
	void stateChanged();
	// The data changed; coalesced to one emission per event-loop turn.
	void dataChanged();

private:
	static QString ownKey() { return QStringLiteral("own"); }
	QString keyOf(const AccountRef &ref) const;
	QString keyOf(qint64 accountId, const QString &realm) const;

	void connectNow();
	void scheduleReconnect(int delayMs, State state = State::Waiting);
	void subscribeAll();
	void subscribe(const AccountRef &ref);
	void onOpened();
	void onText(const QByteArray &text);
	void onFeedError(const protocol::Message &m);
	void onClosed(const QString &reason);
	void onTick();
	void setState(State s, const QString &error = QString());
	void markDataChanged();
	void clearSessions();
	QDateTime computePeriodStart() const;

	Config config_;
	WebSocket *socket_ = nullptr;
	State state_ = State::NoKey;
	QString lastError_;
	protocol::Account account_;
	QList<AccountRef> extras_;
	mutable QHash<QString, Feed> feeds_;
	QDateTime periodStart_;
	int version_ = protocol::kVersion;
	bool fallingBack_ = false; // the next connection is protocol 1, once
	int maxExtra_ = 0;
	bool connected_ = false;
	bool stopped_ = false;
	int attempt_ = 0;
	QTimer reconnectTimer_;
	QTimer tickTimer_;
	QTimer dataTimer_;
	QElapsedTimer sinceReceived_;
	QElapsedTimer sincePing_;
};

} // namespace bss
