#include "catalog-store.h"

#include "net/http.h"

// obs-module.h first: it declares blogva as imported from libobs, which MSVC
// will not accept after plugin-support.h's plain declaration.
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QCryptographicHash>
#include <QFile>
#include <QSaveFile>

namespace bss {

namespace {

constexpr int kRefreshMs = 6 * 60 * 60 * 1000;
constexpr int kRetryMs = 10 * 60 * 1000;

QString cachePath(const char *name)
{
	char *path = obs_module_config_path(name);
	const QString p = QString::fromUtf8(path);
	bfree(path);
	return p;
}

QString sha256(const QByteArray &bytes)
{
	return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

} // namespace

CatalogStore::CatalogStore(QObject *parent) : QObject(parent)
{
	QFile f(cachePath("vehicles.json"));
	if (f.open(QIODevice::ReadOnly)) {
		const QByteArray bytes = f.readAll();
		if (Catalog::parse(bytes, catalog_))
			cachedSha_ = sha256(bytes);
	}
	connect(&timer_, &QTimer::timeout, this, &CatalogStore::refresh);
	timer_.start(kRefreshMs);
}

void CatalogStore::setEnabled(bool enabled)
{
	const bool was = enabled_;
	enabled_ = enabled;
	if (enabled && !was)
		refresh();
}

void CatalogStore::refresh()
{
	if (busy_ || !enabled_)
		return;
	busy_ = true;
	http::get(QUrl(QString::fromLatin1(kManifestUrl)), this,
		  [this](int status, const QByteArray &body, const QString &error) {
			  CatalogPointer pointer;
			  if (status != 200 || !Catalog::parseManifest(body, pointer)) {
				  obs_log(LOG_WARNING, "vehicle manifest: HTTP %d %s", status, qPrintable(error));
				  busy_ = false;
				  retryLater();
				  return;
			  }
			  if (pointer.sha256 == cachedSha_) {
				  busy_ = false;
				  return;
			  }
			  fetchCatalog(pointer);
		  });
}

void CatalogStore::fetchCatalog(const CatalogPointer &pointer)
{
	http::get(QUrl(pointer.url), this, [this, pointer](int status, const QByteArray &body, const QString &error) {
		busy_ = false;
		Catalog fresh;
		// A copy that does not match its published hash is never used.
		if (status != 200 || sha256(body) != pointer.sha256 || !Catalog::parse(body, fresh)) {
			obs_log(LOG_WARNING, "vehicle catalogue: HTTP %d %s", status, qPrintable(error));
			retryLater();
			return;
		}
		catalog_ = fresh;
		cachedSha_ = pointer.sha256;

		char *dir = obs_module_config_path("");
		os_mkdirs(dir);
		bfree(dir);
		QSaveFile f(cachePath("vehicles.json"));
		if (f.open(QIODevice::WriteOnly)) {
			f.write(body);
			f.commit();
		}
		obs_log(LOG_INFO, "vehicle catalogue revision %d, %lld vehicles", catalog_.revision(),
			static_cast<long long>(catalog_.size()));
		emit changed();
	});
}

void CatalogStore::retryLater()
{
	QTimer::singleShot(kRetryMs, this, &CatalogStore::refresh);
}

} // namespace bss
