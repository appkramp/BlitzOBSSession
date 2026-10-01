/*
Blitz Session Stats — World of Tanks Blitz session statistics on stream.
Copyright (C) 2026 AppKramp

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include "plugin.h"

#include "catalog-store.h"
#include "render.h"
#include "stats-client.h"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/text-lookup.h>

#include <QAction>
#include <QFile>
#include <QHash>
#include <QTimer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "World of Tanks Blitz session statistics from stats.appkramp.com";
}

namespace bss {

namespace {

Plugin *g_plugin = nullptr;
Layout g_layout;
QHash<QString, lookup_t *> g_overlayLookups;

QString moduleFile(const char *name)
{
	char *path = obs_module_file(name);
	const QString p = QString::fromUtf8(path);
	bfree(path);
	return p;
}

// data/layout.json, unless the streamer put their own in the plugin's
// configuration directory. A broken override is reported and ignored.
Layout loadLayout()
{
	Layout layout;
	QString error;

	char *custom = obs_module_config_path("layout.json");
	QFile override(QString::fromUtf8(custom));
	bfree(custom);
	if (override.open(QIODevice::ReadOnly)) {
		if (Layout::fromJson(override.readAll(), layout, &error)) {
			obs_log(LOG_INFO, "using %s", qPrintable(override.fileName()));
			return layout;
		}
		obs_log(LOG_WARNING, "%s ignored: %s", qPrintable(override.fileName()), qPrintable(error));
	}

	QFile shipped(moduleFile("layout.json"));
	if (!shipped.open(QIODevice::ReadOnly) || !Layout::fromJson(shipped.readAll(), layout, &error))
		obs_log(LOG_ERROR, "layout.json: %s", qPrintable(error));
	return layout;
}

void onFrontendEvent(enum obs_frontend_event event, void *)
{
	// Networking stops while Qt and the main window still exist.
	if (event == OBS_FRONTEND_EVENT_EXIT && g_plugin && g_plugin->client)
		g_plugin->client->stop();
}

} // namespace

Plugin *plugin()
{
	return g_plugin;
}

QString overlayLanguage()
{
	QString lang = g_plugin && g_plugin->client ? g_plugin->client->config().language : QStringLiteral("auto");
	if (lang == QLatin1String("auto")) {
		const QString obs = QString::fromUtf8(obs_get_locale());
		lang = obs.startsWith(QLatin1String("ru"))   ? QStringLiteral("ru")
		       : obs.startsWith(QLatin1String("uk")) ? QStringLiteral("uk")
							     : QStringLiteral("en");
	}
	return lang;
}

QString overlayText(const QString &key)
{
	const QString lang = overlayLanguage();
	lookup_t *&lookup = g_overlayLookups[lang];
	if (!lookup) {
		const char *locale = lang == QLatin1String("ru")   ? "ru-RU"
				     : lang == QLatin1String("uk") ? "uk-UA"
								   : "en-US";
		lookup = obs_module_load_locale(obs_current_module(), "en-US", locale);
	}
	const char *out = nullptr;
	const QByteArray k = key.toUtf8();
	if (lookup && text_lookup_getstr(lookup, k.constData(), &out))
		return QString::fromUtf8(out);
	return key;
}

QString uiText(const char *key)
{
	return QString::fromUtf8(obs_module_text(key));
}

} // namespace bss

bool obs_module_load(void)
{
	bss::g_layout = bss::loadLayout();
	bss::registerOverlaySource();
	obs_log(LOG_INFO, "loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	using namespace bss;

	g_plugin = new Plugin;
	g_plugin->layout = g_layout;
	g_plugin->renderer = new Renderer(moduleFile("icons"));
	g_plugin->catalog = new CatalogStore();
	g_plugin->client = new StatsClient();

	QObject::connect(g_plugin->client, &StatsClient::dataChanged, [] { renderAllSources(); });
	QObject::connect(g_plugin->catalog, &CatalogStore::changed, [] { renderAllSources(); });

	auto *action = static_cast<QAction *>(obs_frontend_add_tools_menu_qaction(obs_module_text("Menu.Settings")));
	QObject::connect(action, &QAction::triggered, [] { openSettings(); });
	obs_frontend_add_event_callback(onFrontendEvent, nullptr);

	auto *scaleTimer = new QTimer(g_plugin->client);
	QObject::connect(scaleTimer, &QTimer::timeout, [] { normalizeSceneScales(); });
	scaleTimer->start(250);

	g_plugin->catalog->refresh();
	g_plugin->client->setConfig(Config::load());
	renderAllSources();
}

void obs_module_unload(void)
{
	using namespace bss;

	obs_frontend_remove_event_callback(onFrontendEvent, nullptr);
	if (g_plugin) {
		delete g_plugin->client;
		delete g_plugin->catalog;
		delete g_plugin->renderer;
		delete g_plugin;
		g_plugin = nullptr;
	}
	for (lookup_t *lookup : std::as_const(g_overlayLookups))
		text_lookup_destroy(lookup);
	g_overlayLookups.clear();
	obs_log(LOG_INFO, "unloaded");
}
