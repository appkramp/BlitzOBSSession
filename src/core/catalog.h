#pragma once

// The vehicle catalogue the mobile application uses, published on
// cdn.appkramp.com: names in en/ru/uk, tier and class per tank_id.
//
//   manifest.json          small, polled; points at the catalogue
//   vehicles.v<N>.json     immutable once published

#include <QByteArray>
#include <QHash>
#include <QString>

namespace bss {

struct Vehicle {
	int tier = 0;
	QString type; // lightTank, mediumTank, heavyTank, AT-SPG
	QHash<QString, QString> names;

	// Falls back to English, then to any name present.
	QString name(const QString &language) const;
};

struct CatalogPointer {
	QString url;
	QString sha256;
	int revision = 0;
};

class Catalog {
public:
	static bool parseManifest(const QByteArray &json, CatalogPointer &out);
	static bool parse(const QByteArray &json, Catalog &out);

	const Vehicle *find(int tankId) const;
	int revision() const { return revision_; }
	qsizetype size() const { return vehicles_.size(); }

private:
	int revision_ = 0;
	QHash<int, Vehicle> vehicles_;
};

} // namespace bss
