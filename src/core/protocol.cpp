#include "protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace bss::protocol {

namespace {

QByteArray encode(const QJsonObject &obj)
{
	return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

Account readAccount(const QJsonObject &obj)
{
	Account a;
	a.accountId = static_cast<qint64>(obj.value(QStringLiteral("account_id")).toDouble());
	a.nickname = obj.value(QStringLiteral("nickname")).toString();
	a.clanTag = obj.value(QStringLiteral("clan_tag")).toString();
	for (const QJsonValue r : obj.value(QStringLiteral("realms")).toArray())
		a.realms.append(r.toString());
	return a;
}

} // namespace

QByteArray hello(const QString &key, const QString &client, int version)
{
	return encode({{QStringLiteral("type"), QStringLiteral("hello")},
		       {QStringLiteral("protocol"), version},
		       {QStringLiteral("key"), key},
		       {QStringLiteral("client"), client}});
}

QByteArray subscribe(qint64 accountId, const QString &realm, const QDateTime &since, qint64 afterId)
{
	QJsonObject o{{QStringLiteral("type"), QStringLiteral("subscribe")},
		      {QStringLiteral("realm"), realm},
		      {QStringLiteral("since"), since.toUTC().toString(Qt::ISODate)},
		      {QStringLiteral("after_id"), static_cast<double>(afterId)}};
	// The key's own account goes without an id, which protocol 1 also reads.
	if (accountId != 0)
		o.insert(QStringLiteral("account_id"), static_cast<double>(accountId));
	return encode(o);
}

QByteArray unsubscribe(qint64 accountId, const QString &realm)
{
	return encode({{QStringLiteral("type"), QStringLiteral("unsubscribe")},
		       {QStringLiteral("account_id"), static_cast<double>(accountId)},
		       {QStringLiteral("realm"), realm}});
}

QByteArray ping()
{
	return encode({{QStringLiteral("type"), QStringLiteral("ping")}});
}

bool parse(const QByteArray &text, Message &out)
{
	const QJsonDocument doc = QJsonDocument::fromJson(text);
	if (!doc.isObject())
		return false;
	const QJsonObject obj = doc.object();
	const QString type = obj.value(QStringLiteral("type")).toString();
	if (type.isEmpty())
		return false;

	Message m;
	m.accountId = static_cast<qint64>(obj.value(QStringLiteral("account_id")).toDouble());
	m.realm = obj.value(QStringLiteral("realm")).toString();
	if (type == QLatin1String("welcome") || type == QLatin1String("account")) {
		m.type = type == QLatin1String("welcome") ? Message::Type::Welcome : Message::Type::Account;
		m.account = readAccount(obj.value(QStringLiteral("account")).toObject());
		m.maxExtraAccounts = obj.value(QStringLiteral("max_extra_accounts")).toInt();
		for (const QJsonValue r : obj.value(QStringLiteral("realms")).toArray())
			m.serverRealms.append(r.toString());
	} else if (type == QLatin1String("subscribed")) {
		m.type = Message::Type::Subscribed;
		m.nickname = obj.value(QStringLiteral("nickname")).toString();
		m.clanTag = obj.value(QStringLiteral("clan_tag")).toString();
		m.collecting = obj.value(QStringLiteral("collecting")).toBool();
		m.hasReading = obj.value(QStringLiteral("has_reading")).toBool(true);
	} else if (type == QLatin1String("unsubscribed")) {
		m.type = Message::Type::Unsubscribed;
	} else if (type == QLatin1String("history") || type == QLatin1String("battles")) {
		m.type = type == QLatin1String("history") ? Message::Type::History : Message::Type::Battles;
		m.hasMore = obj.value(QStringLiteral("has_more")).toBool();
		for (const QJsonValue v : obj.value(QStringLiteral("battles")).toArray()) {
			BattleRecord r;
			if (BattleRecord::fromJson(v.toObject(), r))
				m.battles.append(r);
		}
	} else if (type == QLatin1String("error")) {
		m.type = Message::Type::Error;
		m.errorCode = obj.value(QStringLiteral("code")).toString();
		m.errorMessage = obj.value(QStringLiteral("message")).toString();
		m.fatal = obj.value(QStringLiteral("fatal")).toBool();
		m.retryAfter = obj.value(QStringLiteral("retry_after")).toInt();
	} else if (type == QLatin1String("pong")) {
		m.type = Message::Type::Pong;
	}
	out = m;
	return true;
}

bool looksLikeKey(const QString &key)
{
	static const QRegularExpression re(QStringLiteral("^ovk_[A-Za-z0-9_-]{32,64}$"));
	return re.match(key).hasMatch();
}

int reconnectDelayMs(int attempt, double jitter01)
{
	const double base = std::min(60000.0, 1000.0 * std::pow(2.0, std::clamp(attempt, 0, 6)));
	return static_cast<int>(base * (1.0 + 0.2 * std::clamp(jitter01, 0.0, 1.0)));
}

QDateTime startOfLocalDay(const QDateTime &now)
{
	return now.toLocalTime().date().startOfDay();
}

} // namespace bss::protocol
