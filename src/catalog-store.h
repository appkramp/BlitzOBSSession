#pragma once

// Keeps the vehicle catalogue (core/catalog.h) current: loads the copy cached
// in the plugin's configuration directory at once, then checks the manifest on
// cdn.appkramp.com and downloads a new catalogue when its hash changed.

#include "core/catalog.h"

#include <QObject>
#include <QTimer>

namespace bss {

class CatalogStore : public QObject {
	Q_OBJECT

public:
	static constexpr const char *kManifestUrl = "https://cdn.appkramp.com/manifest.json";

	explicit CatalogStore(QObject *parent = nullptr);

	const Catalog &catalog() const { return catalog_; }
	void refresh();

signals:
	void changed();

private:
	void fetchCatalog(const CatalogPointer &pointer);
	void retryLater();

	Catalog catalog_;
	QString cachedSha_;
	bool busy_ = false;
	QTimer timer_;
};

} // namespace bss
