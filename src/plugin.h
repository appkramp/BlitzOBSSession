#pragma once

// What the parts of the plugin share: the template, the connection, the
// catalogue, the renderer, and the overlay's translations. Created when OBS
// has finished loading modules; used on the Qt main thread only.

#include "core/layout.h"

#include <QString>

namespace bss {

class CatalogStore;
class Renderer;
class StatsClient;

struct Plugin {
	Layout layout;
	StatsClient *client = nullptr;
	CatalogStore *catalog = nullptr;
	Renderer *renderer = nullptr;
};

// Null before obs_module_post_load and after unload.
Plugin *plugin();

// The overlay's language as a catalogue key: ru, en or uk.
QString overlayLanguage();
// A locale key in the overlay's language, which may differ from OBS's.
QString overlayText(const QString &key);
// A locale key in OBS's own language.
QString uiText(const char *key);

void openSettings();
void registerOverlaySource();
// Repaints every overlay source; called when the data, the catalogue or the
// language changed.
void renderAllSources();
// Turns a source stretched in a scene into a wider table at scale 1, so the
// font keeps the size set in the properties. Called on a timer.
void normalizeSceneScales();

} // namespace bss
