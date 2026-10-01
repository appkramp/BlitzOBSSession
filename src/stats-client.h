#pragma once

// The connection to the statistics server (docs/overlay-protocol.md) and the
// session it fills. One per OBS instance, on the Qt main thread; every source
// reads from it.

#include "config.h"
#include "core/protocol.h"
#include "core/session.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

namespace bss {

class WebSocket;

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

	State state() const { return state_; }
	QString lastError() const { return lastError_; }
	const protocol::Account &account() const { return account_; }
	const Session &session() const { return session_; }
	QDateTime periodStart() const { return periodStart_; }

signals:
	void stateChanged();
	// The session changed; coalesced to one emission per event-loop turn.
	void dataChanged();

private:
	void connectNow();
	void scheduleReconnect(int delayMs, State state = State::Waiting);
	void subscribe();
	void onOpened();
	void onText(const QByteArray &text);
	void onClosed(const QString &reason);
	void onTick();
	void setState(State s, const QString &error = QString());
	void markDataChanged();
	QDateTime computePeriodStart() const;

	Config config_;
	WebSocket *socket_ = nullptr;
	State state_ = State::NoKey;
	QString lastError_;
	protocol::Account account_;
	Session session_;
	QDateTime periodStart_;
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
