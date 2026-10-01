#pragma once

// docs/overlay-protocol.md, as data: building the client's messages and reading
// the server's. No I/O here.

#include "session.h"

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace bss::protocol {

constexpr int kVersion = 1;

QByteArray hello(const QString &key, const QString &client);
QByteArray subscribe(const QString &realm, const QDateTime &since, qint64 afterId);
QByteArray ping();

struct Account {
	qint64 accountId = 0;
	QString nickname;
	QString clanTag;
	QStringList realms;
};

struct Message {
	enum class Type { Unknown, Welcome, History, Battles, Account, Error, Pong };

	Type type = Type::Unknown;
	Account account;             // welcome, account
	QString realm;               // history, battles
	QList<BattleRecord> battles; // history, battles
	bool hasMore = false;        // history
	QString errorCode;           // error
	QString errorMessage;        // error
	bool fatal = false;          // error
	int retryAfter = 0;          // error, seconds
};

// False when the text is not a JSON object with a `type`.
bool parse(const QByteArray &text, Message &out);

// Keys are `ovk_` and 32–64 characters of [A-Za-z0-9_-]. Only a hint for the
// settings form; the server decides validity.
bool looksLikeKey(const QString &key);

// Delay before reconnect attempt `attempt` (0-based): 1, 2, 4 … 60 seconds,
// with up to 20% jitter so a server restart is not met by every client at once.
int reconnectDelayMs(int attempt, double jitter01);

// The start of "today" as the streamer's clock sees it, at `now`.
QDateTime startOfLocalDay(const QDateTime &now);

} // namespace bss::protocol
