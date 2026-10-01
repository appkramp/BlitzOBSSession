#include "catalog.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace bss {

QString Vehicle::name(const QString &language) const
{
	QString n = names.value(language);
	if (n.isEmpty())
		n = names.value(QStringLiteral("en"));
	if (n.isEmpty() && !names.isEmpty())
		n = names.begin().value();
	return n;
}

bool Catalog::parseManifest(const QByteArray &json, CatalogPointer &out)
{
	const QJsonObject root = QJsonDocument::fromJson(json).object();
	if (root.value(QStringLiteral("schema_version")).toInt() != 1)
		return false;
	const QJsonObject catalog = root.value(QStringLiteral("catalog")).toObject();
	out.url = catalog.value(QStringLiteral("url")).toString();
	out.sha256 = catalog.value(QStringLiteral("sha256")).toString().toLower();
	out.revision = root.value(QStringLiteral("revision")).toInt();
	return out.url.startsWith(QLatin1String("https://")) && out.sha256.size() == 64;
}

bool Catalog::parse(const QByteArray &json, Catalog &out)
{
	const QJsonObject root = QJsonDocument::fromJson(json).object();
	if (root.value(QStringLiteral("schema_version")).toInt() != 1)
		return false;

	Catalog catalog;
	catalog.revision_ = root.value(QStringLiteral("revision")).toInt();
	const QJsonObject vehicles = root.value(QStringLiteral("vehicles")).toObject();
	for (auto it = vehicles.begin(); it != vehicles.end(); ++it) {
		bool ok = false;
		const int id = it.key().toInt(&ok);
		if (!ok)
			continue;
		const QJsonObject obj = it.value().toObject();
		Vehicle v;
		v.tier = obj.value(QStringLiteral("tier")).toInt();
		v.type = obj.value(QStringLiteral("type")).toString();
		const QJsonObject names = obj.value(QStringLiteral("names")).toObject();
		for (auto n = names.begin(); n != names.end(); ++n)
			v.names.insert(n.key(), n.value().toString());
		catalog.vehicles_.insert(id, v);
	}
	if (catalog.vehicles_.isEmpty())
		return false;
	out = catalog;
	return true;
}

const Vehicle *Catalog::find(int tankId) const
{
	auto it = vehicles_.constFind(tankId);
	return it == vehicles_.constEnd() ? nullptr : &it.value();
}

} // namespace bss
