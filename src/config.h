#pragma once

// The plugin's global settings: the key, the realm, the period, the overlay
// language. Stored in the plugin's own configuration file, never in a scene
// collection — collections get exported and shared, and the key must not
// travel with them.

#include <QDateTime>
#include <QString>

namespace bss {

struct Config {
	static constexpr const char *kDefaultServer = "wss://stats.appkramp.com/v1/overlay/ws";

	QString key;
	QString realm = QStringLiteral("eu"); // eu, com, asia
	enum class Period { Today, Since };
	Period period = Period::Today;
	QDateTime since;                           // for Period::Since, local time
	QString language = QStringLiteral("auto"); // auto, ru, en, uk — of the overlay text
	QString server = QString::fromLatin1(kDefaultServer);

	static Config load();
	bool save() const;

	bool sameConnection(const Config &o) const { return key == o.key && server == o.server; }
	bool sameSubscription(const Config &o) const
	{
		return realm == o.realm && period == o.period && (period == Period::Today || since == o.since);
	}
};

} // namespace bss
