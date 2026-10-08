#include "config.h"

#include "core/protocol.h"

#include <obs-module.h>
#include <util/platform.h>

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace bss {

namespace {

QString configFile()
{
	char *path = obs_module_config_path("config.json");
	const QString p = QString::fromUtf8(path);
	bfree(path);
	return p;
}

} // namespace

Config Config::load()
{
	Config c;
	QFile f(configFile());
	if (!f.open(QIODevice::ReadOnly))
		return c;
	const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
	// A key saved by an earlier version may carry what was pasted around it.
	c.key = protocol::extractKey(o.value(QStringLiteral("key")).toString());
	c.realm = o.value(QStringLiteral("realm")).toString(c.realm);
	c.period = o.value(QStringLiteral("period")).toString() == QLatin1String("since") ? Period::Since
											  : Period::Today;
	c.since = QDateTime::fromString(o.value(QStringLiteral("since")).toString(), Qt::ISODate);
	c.language = o.value(QStringLiteral("language")).toString(c.language);
	c.server = o.value(QStringLiteral("server")).toString(c.server);
	if (c.server.isEmpty())
		c.server = QString::fromLatin1(kDefaultServer);
	return c;
}

bool Config::save() const
{
	char *dir = obs_module_config_path("");
	os_mkdirs(dir);
	bfree(dir);

	const QJsonObject o{
		{QStringLiteral("key"), key},
		{QStringLiteral("realm"), realm},
		{QStringLiteral("period"), period == Period::Since ? QStringLiteral("since") : QStringLiteral("today")},
		{QStringLiteral("since"), since.isValid() ? since.toString(Qt::ISODate) : QString()},
		{QStringLiteral("language"), language},
		{QStringLiteral("server"), server},
	};
	QSaveFile f(configFile());
	if (!f.open(QIODevice::WriteOnly))
		return false;
	f.write(QJsonDocument(o).toJson());
	return f.commit();
}

} // namespace bss
