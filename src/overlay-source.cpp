// The "Blitz Session Stats" source: a texture painted on the Qt main thread
// from the shared session, uploaded on the graphics thread.
//
// OBS calls a source from several threads: update and properties from the UI,
// render on the graphics thread, destroy from wherever the last reference is
// dropped. The source's data therefore lives in a shared_ptr; the main thread
// reaches it through a weak_ptr, and everything the threads share is behind
// the source's mutex.

#include "plugin.h"

#include "catalog-store.h"
#include "core/table.h"
#include "render.h"
#include "stats-client.h"
#include "ui/columns-dialog.h"
#include "ui/ranges-dialog.h"

#include <obs-module.h>
#include <plugin-support.h>

#include <QApplication>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QImage>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace bss {

namespace {

constexpr const char *kSourceId = "blitz_session_stats";

constexpr const char *kRows = "rows";
constexpr const char *kSortBy = "sort_by";
constexpr const char *kSortDir = "sort_dir";
constexpr const char *kColumnOrder = "column_order";
constexpr const char *kMaxRows = "max_rows";
constexpr const char *kWidth = "width";
constexpr const char *kShowHeader = "show_header";
constexpr const char *kHeaderPlace = "header_place";
constexpr const char *kShowTotal = "show_total";
constexpr const char *kFont = "font";
constexpr const char *kColorHeader = "color_header";
constexpr const char *kColorTotal = "color_total";
constexpr const char *kBlockColor = "block_color";
constexpr const char *kBlockOpacity = "block_opacity";
constexpr const char *kBorderColor = "border_color";
constexpr const char *kBorderWidth = "border_width";
constexpr const char *kCornerRadius = "corner_radius";
constexpr const char *kOpacity = "opacity";
constexpr const char *kPaddingX = "padding_x";
constexpr const char *kPaddingY = "padding_y";
constexpr const char *kColumnSpacing = "column_spacing";
constexpr const char *kRowSpacing = "row_spacing";
constexpr const char *kTanksGroup = "tanks";

QByteArray columnKey(const QString &id)
{
	return QByteArrayLiteral("col_") + id.toUtf8();
}

QByteArray colorKey(const QString &id)
{
	return QByteArrayLiteral("color_col_") + id.toUtf8();
}

// The colour a column's data starts with: the tier a little dimmer than the
// rest, so the name and the numbers stand out.
QColor defaultColumnColor(const Column &c)
{
	return c.kind == Column::Kind::Tier ? QColor(200, 206, 216) : QColor(255, 255, 255);
}

QByteArray rangesKey(const QString &id)
{
	return QByteArrayLiteral("ranges_") + id.toUtf8();
}

// A column's colour bands: the source's own once it has saved any (an empty
// string meaning none), otherwise the template's.
QList<ValueRange> columnRanges(const Column &c, obs_data_t *settings)
{
	const QByteArray key = rangesKey(c.id);
	if (obs_data_has_user_value(settings, key.constData()))
		return parseRanges(QString::fromUtf8(obs_data_get_string(settings, key.constData())));
	return c.ranges;
}

QByteArray tankKey(int tankId)
{
	return QByteArrayLiteral("tank_") + QByteArray::number(tankId);
}

// OBS colours are 0xAABBGGRR.
QColor toColor(long long abgr)
{
	const auto v = static_cast<quint32>(abgr);
	return QColor(static_cast<int>(v & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>((v >> 16) & 0xFF),
		      static_cast<int>((v >> 24) & 0xFF));
}

long long fromColor(const QColor &c)
{
	return static_cast<long long>(static_cast<quint32>(c.red()) | static_cast<quint32>(c.green()) << 8 |
				      static_cast<quint32>(c.blue()) << 16 | static_cast<quint32>(c.alpha()) << 24);
}

QString columnLabel(const Column &c)
{
	return c.label.isEmpty() ? c.id : uiText(c.label.toUtf8().constData());
}

// Every column id, in the order this source shows them.
QStringList columnOrder(const Layout &layout, obs_data_t *settings)
{
	const QString saved = QString::fromUtf8(obs_data_get_string(settings, kColumnOrder));
	return orderedColumnIds(layout, saved.split(QLatin1Char(','), Qt::SkipEmptyParts));
}

const char *defaultFontFace()
{
#if defined(_WIN32)
	return "Segoe UI";
#elif defined(__APPLE__)
	return "Helvetica Neue";
#else
	return "Sans Serif";
#endif
}

struct OverlaySource {
	obs_source_t *source = nullptr;

	std::mutex mu;
	TableOptions options;
	Style style;
	QImage pending;
	bool hasPending = false;

	gs_texture_t *texture = nullptr; // graphics thread only
	std::atomic<uint32_t> cx{0}, cy{0};
};

std::mutex g_registryMu;
std::vector<std::weak_ptr<OverlaySource>> g_registry;

void renderSource(OverlaySource &s)
{
	Plugin *p = plugin();
	if (!p || !p->renderer)
		return;

	TableOptions options;
	Style style;
	{
		std::lock_guard<std::mutex> lock(s.mu);
		options = s.options;
		style = s.style;
	}
	options.language = overlayLanguage();

	const Table table = buildTable(p->layout, p->client->session(), p->catalog->catalog(), options,
				       [](const QString &key) { return overlayText(key); });
	QImage image = p->renderer->render(table, style);

	std::lock_guard<std::mutex> lock(s.mu);
	s.cx = static_cast<uint32_t>(image.width());
	s.cy = static_cast<uint32_t>(image.height());
	s.pending = std::move(image);
	s.hasPending = true;
}

void requestRender(const std::weak_ptr<OverlaySource> &weak)
{
	if (!QCoreApplication::instance())
		return;
	QMetaObject::invokeMethod(
		QCoreApplication::instance(),
		[weak] {
			if (auto s = weak.lock())
				renderSource(*s);
		},
		Qt::QueuedConnection);
}

void readSettings(OverlaySource &s, obs_data_t *settings)
{
	TableOptions o;
	const QString rows = QString::fromUtf8(obs_data_get_string(settings, kRows));
	o.rows = rows == QLatin1String("selected") ? TableOptions::Rows::Selected
		 : rows == QLatin1String("totals") ? TableOptions::Rows::TotalsOnly
						   : TableOptions::Rows::All;
	o.sortBy = QString::fromUtf8(obs_data_get_string(settings, kSortBy));
	o.ascending = QString::fromUtf8(obs_data_get_string(settings, kSortDir)) == QLatin1String("asc");
	o.maxRows = static_cast<int>(obs_data_get_int(settings, kMaxRows));
	o.showHeader = obs_data_get_bool(settings, kShowHeader);
	o.showTotal = obs_data_get_bool(settings, kShowTotal);
	if (Plugin *p = plugin()) {
		for (const QString &id : columnOrder(p->layout, settings))
			if (obs_data_get_bool(settings, columnKey(id).constData()))
				o.columns.append(id);
	}
	for (obs_data_item_t *item = obs_data_first(settings); item; obs_data_item_next(&item)) {
		const QString name = QString::fromUtf8(obs_data_item_get_name(item));
		if (name.startsWith(QLatin1String("tank_")) && obs_data_item_gettype(item) == OBS_DATA_BOOLEAN &&
		    obs_data_item_get_bool(item))
			o.selectedTanks.insert(name.mid(5).toInt());
	}

	Style st;
	obs_data_t *font = obs_data_get_obj(settings, kFont);
	st.font = QFont(QString::fromUtf8(obs_data_get_string(font, "face")));
	st.font.setStyleName(QString::fromUtf8(obs_data_get_string(font, "style")));
	st.font.setPixelSize(std::max(6, static_cast<int>(obs_data_get_int(font, "size"))));
	const long long flags = obs_data_get_int(font, "flags");
	st.font.setBold(flags & OBS_FONT_BOLD);
	st.font.setItalic(flags & OBS_FONT_ITALIC);
	st.font.setUnderline(flags & OBS_FONT_UNDERLINE);
	st.font.setStrikeOut(flags & OBS_FONT_STRIKEOUT);
	obs_data_release(font);

	if (Plugin *p = plugin()) {
		for (const Column &c : p->layout.columns()) {
			st.columnColors.insert(c.id, toColor(obs_data_get_int(settings, colorKey(c.id).constData())));
			if (c.kind == Column::Kind::Value)
				st.ranges.insert(c.id, columnRanges(c, settings));
		}
	}
	st.header = toColor(obs_data_get_int(settings, kColorHeader));
	st.labelsInside = QString::fromUtf8(obs_data_get_string(settings, kHeaderPlace)) == QLatin1String("inside");
	st.minWidth = static_cast<int>(obs_data_get_int(settings, kWidth));
	st.total = toColor(obs_data_get_int(settings, kColorTotal));
	// The block's colour and its transparency are set apart, as the streamer
	// thinks of them.
	st.block = toColor(obs_data_get_int(settings, kBlockColor));
	st.block.setAlphaF(
		static_cast<float>(std::clamp<long long>(obs_data_get_int(settings, kBlockOpacity), 0, 100)) / 100.0f);
	st.border = toColor(obs_data_get_int(settings, kBorderColor));
	st.borderWidth = static_cast<int>(obs_data_get_int(settings, kBorderWidth));
	st.radius = static_cast<int>(obs_data_get_int(settings, kCornerRadius));
	st.opacity = static_cast<double>(obs_data_get_int(settings, kOpacity)) / 100.0;
	st.paddingX = static_cast<int>(obs_data_get_int(settings, kPaddingX));
	st.paddingY = static_cast<int>(obs_data_get_int(settings, kPaddingY));
	st.columnSpacing = static_cast<int>(obs_data_get_int(settings, kColumnSpacing));
	st.rowSpacing = static_cast<int>(obs_data_get_int(settings, kRowSpacing));

	std::lock_guard<std::mutex> lock(s.mu);
	s.options = o;
	s.style = st;
}

std::shared_ptr<OverlaySource> *holder(void *data)
{
	return static_cast<std::shared_ptr<OverlaySource> *>(data);
}

const char *getName(void *)
{
	return obs_module_text("BlitzSessionStats");
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto s = std::make_shared<OverlaySource>();
	s->source = source;
	readSettings(*s, settings);
	{
		std::lock_guard<std::mutex> lock(g_registryMu);
		g_registry.push_back(s);
	}
	requestRender(s);
	return new std::shared_ptr<OverlaySource>(std::move(s));
}

void destroy(void *data)
{
	std::shared_ptr<OverlaySource> s = std::move(*holder(data));
	delete holder(data);
	{
		std::lock_guard<std::mutex> lock(g_registryMu);
		g_registry.erase(std::remove_if(g_registry.begin(), g_registry.end(),
						[&](const std::weak_ptr<OverlaySource> &w) {
							auto locked = w.lock();
							return !locked || locked == s;
						}),
				 g_registry.end());
	}
	if (s->texture) {
		obs_enter_graphics();
		gs_texture_destroy(s->texture);
		obs_leave_graphics();
		s->texture = nullptr;
	}
}

void update(void *data, obs_data_t *settings)
{
	auto &s = *holder(data);
	readSettings(*s, settings);
	requestRender(s);
}

void getDefaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, kRows, "all");
	obs_data_set_default_string(settings, kSortBy, "");
	obs_data_set_default_string(settings, kSortDir, "desc");
	obs_data_set_default_string(settings, kColumnOrder, "");
	obs_data_set_default_int(settings, kMaxRows, 0);
	obs_data_set_default_int(settings, kWidth, 0);
	obs_data_set_default_bool(settings, kShowHeader, true);
	obs_data_set_default_string(settings, kHeaderPlace, "top");
	obs_data_set_default_bool(settings, kShowTotal, true);
	if (Plugin *p = plugin()) {
		for (const Column &c : p->layout.columns())
			obs_data_set_default_bool(settings, columnKey(c.id).constData(), c.visibleByDefault);
	}

	obs_data_t *font = obs_data_create();
	obs_data_set_default_string(font, "face", defaultFontFace());
	obs_data_set_default_string(font, "style", "Regular");
	obs_data_set_default_int(font, "size", 28);
	obs_data_set_default_int(font, "flags", 0);
	obs_data_set_default_obj(settings, kFont, font);
	obs_data_release(font);

	const Style st;
	if (Plugin *p = plugin()) {
		for (const Column &c : p->layout.columns())
			obs_data_set_default_int(settings, colorKey(c.id).constData(),
						 fromColor(defaultColumnColor(c)));
	}
	obs_data_set_default_int(settings, kColorHeader, fromColor(st.header));
	obs_data_set_default_int(settings, kColorTotal, fromColor(st.total));
	QColor block = st.block;
	block.setAlpha(255);
	obs_data_set_default_int(settings, kBlockColor, fromColor(block));
	obs_data_set_default_int(settings, kBlockOpacity, std::lround(st.block.alphaF() * 100));
	obs_data_set_default_int(settings, kBorderColor, fromColor(st.border));
	obs_data_set_default_int(settings, kBorderWidth, st.borderWidth);
	obs_data_set_default_int(settings, kCornerRadius, st.radius);
	obs_data_set_default_int(settings, kOpacity, 100);
	obs_data_set_default_int(settings, kPaddingX, st.paddingX);
	obs_data_set_default_int(settings, kPaddingY, st.paddingY);
	obs_data_set_default_int(settings, kColumnSpacing, st.columnSpacing);
	obs_data_set_default_int(settings, kRowSpacing, st.rowSpacing);
}

bool layoutModified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const QString rows = QString::fromUtf8(obs_data_get_string(settings, kRows));
	const bool totals = rows == QLatin1String("totals");
	const bool sorted = *obs_data_get_string(settings, kSortBy) != '\0';
	obs_property_set_visible(obs_properties_get(props, kTanksGroup), rows == QLatin1String("selected"));
	obs_property_set_visible(obs_properties_get(props, kSortBy), !totals);
	obs_property_set_visible(obs_properties_get(props, kSortDir), !totals && sorted);
	obs_property_set_visible(obs_properties_get(props, kMaxRows), !totals);
	obs_property_set_visible(obs_properties_get(props, kShowTotal), !totals);
	obs_property_set_visible(obs_properties_get(props, kHeaderPlace), obs_data_get_bool(settings, kShowHeader));
	return true;
}

// The "Columns…" button: which columns, in what order.
bool editColumns(obs_properties_t *, obs_property_t *, void *priv)
{
	auto *source = static_cast<obs_source_t *>(priv);
	Plugin *p = plugin();
	if (!source || !p)
		return false;

	obs_data_t *settings = obs_source_get_settings(source);
	QList<ColumnsDialog::Item> items;
	for (const QString &id : columnOrder(p->layout, settings)) {
		for (const Column &c : p->layout.columns())
			if (c.id == id)
				items.append(
					{id, columnLabel(c), obs_data_get_bool(settings, columnKey(id).constData())});
	}

	ColumnsDialog dialog(items, QApplication::activeWindow());
	const bool accepted = dialog.exec() == QDialog::Accepted;
	if (accepted) {
		QStringList order;
		for (const ColumnsDialog::Item &it : dialog.items()) {
			order.append(it.id);
			obs_data_set_bool(settings, columnKey(it.id).constData(), it.visible);
		}
		obs_data_set_string(settings, kColumnOrder, order.join(QLatin1Char(',')).toUtf8().constData());
		obs_source_update(source, settings);
	}
	obs_data_release(settings);
	return accepted;
}

// The "Colour by value…" button.
bool editRanges(obs_properties_t *, obs_property_t *, void *priv)
{
	auto *source = static_cast<obs_source_t *>(priv);
	Plugin *p = plugin();
	if (!source || !p)
		return false;

	obs_data_t *settings = obs_source_get_settings(source);
	QList<RangesDialog::ColumnInfo> columns;
	QString selected;
	for (const QString &id : columnOrder(p->layout, settings)) {
		for (const Column &c : p->layout.columns()) {
			if (c.id != id || c.kind != Column::Kind::Value)
				continue;
			columns.append(
				{c.id, columnLabel(c), c.decimals, c.suffix, c.ranges, columnRanges(c, settings)});
			if (selected.isEmpty() && !c.ranges.isEmpty())
				selected = c.id;
		}
	}

	RangesDialog dialog(columns, selected, QApplication::activeWindow());
	const bool accepted = dialog.exec() == QDialog::Accepted;
	if (accepted) {
		const auto edited = dialog.edited();
		for (auto it = edited.begin(); it != edited.end(); ++it)
			obs_data_set_string(settings, rangesKey(it.key()).constData(),
					    formatRanges(it.value()).toUtf8().constData());
		for (const QString &id : dialog.resetToDefault())
			obs_data_unset_user_value(settings, rangesKey(id).constData());
		obs_source_update(source, settings);
	}
	obs_data_release(settings);
	return false;
}

obs_properties_t *getProperties(void *data)
{
	obs_source_t *source = data ? (*holder(data))->source : nullptr;
	obs_properties_t *props = obs_properties_create();
	Plugin *p = plugin();

	obs_properties_add_button2(
		props, "connection", obs_module_text("Prop.Connection"),
		[](obs_properties_t *, obs_property_t *, void *) {
			openSettings();
			return false;
		},
		nullptr);

	obs_property_t *rows = obs_properties_add_list(props, kRows, obs_module_text("Prop.Rows"), OBS_COMBO_TYPE_LIST,
						       OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(rows, obs_module_text("Rows.All"), "all");
	obs_property_list_add_string(rows, obs_module_text("Rows.Selected"), "selected");
	obs_property_list_add_string(rows, obs_module_text("Rows.Totals"), "totals");
	obs_property_set_modified_callback(rows, layoutModified);

	// The vehicles of the current session: the streamer picks from what is
	// actually being played.
	obs_properties_t *tanks = obs_properties_create();
	const QString lang = QString::fromUtf8(obs_get_locale()).left(2);
	if (p && !p->client->session().isEmpty()) {
		for (const TankTotals &t : p->client->session().ordered(Session::Order::LastPlayed)) {
			const Vehicle *v = p->catalog->catalog().find(t.tankId);
			const QString label = v ? QStringLiteral("%1 %2").arg(romanTier(v->tier), v->name(lang))
						: QStringLiteral("#%1").arg(t.tankId);
			obs_properties_add_bool(tanks, tankKey(t.tankId).constData(), label.toUtf8().constData());
		}
	} else {
		obs_properties_add_text(tanks, "tanks_empty", obs_module_text("Prop.TanksEmpty"), OBS_TEXT_INFO);
	}
	obs_properties_add_button2(
		tanks, "tanks_refresh", obs_module_text("Prop.RefreshTanks"),
		[](obs_properties_t *, obs_property_t *, void *) { return true; }, nullptr);
	obs_properties_add_group(props, kTanksGroup, obs_module_text("Prop.Tanks"), OBS_GROUP_NORMAL, tanks);

	obs_property_t *sortBy = obs_properties_add_list(props, kSortBy, obs_module_text("Prop.Sort"),
							 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sortBy, obs_module_text("Sort.Last"), "");
	if (p) {
		for (const Column &c : p->layout.columns())
			obs_property_list_add_string(sortBy, columnLabel(c).toUtf8().constData(),
						     c.id.toUtf8().constData());
	}
	obs_property_set_modified_callback(sortBy, layoutModified);
	obs_property_t *sortDir = obs_properties_add_list(props, kSortDir, obs_module_text("Prop.SortDir"),
							  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sortDir, obs_module_text("SortDir.Desc"), "desc");
	obs_property_list_add_string(sortDir, obs_module_text("SortDir.Asc"), "asc");

	obs_property_t *width = obs_properties_add_int(props, kWidth, obs_module_text("Prop.Width"), 0, 4000, 10);
	obs_property_int_set_suffix(width, " px");
	obs_property_set_long_description(width, obs_module_text("Prop.WidthHint"));
	obs_properties_add_int(props, kMaxRows, obs_module_text("Prop.MaxRows"), 0, 100, 1);
	obs_property_t *showHeader = obs_properties_add_bool(props, kShowHeader, obs_module_text("Prop.ShowHeader"));
	obs_property_set_modified_callback(showHeader, layoutModified);
	obs_property_t *place = obs_properties_add_list(props, kHeaderPlace, obs_module_text("Prop.HeaderPlace"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(place, obs_module_text("HeaderPlace.Top"), "top");
	obs_property_list_add_string(place, obs_module_text("HeaderPlace.Inside"), "inside");
	obs_properties_add_bool(props, kShowTotal, obs_module_text("Prop.ShowTotal"));

	// The columns shown, in order, and the button that changes them.
	obs_properties_t *columns = obs_properties_create();
	if (p && source) {
		obs_data_t *settings = obs_source_get_settings(source);
		QStringList shown;
		for (const QString &id : columnOrder(p->layout, settings)) {
			if (!obs_data_get_bool(settings, columnKey(id).constData()))
				continue;
			for (const Column &c : p->layout.columns())
				if (c.id == id)
					shown.append(columnLabel(c));
		}
		obs_data_release(settings);
		const QString text = shown.isEmpty() ? QStringLiteral("—") : shown.join(QStringLiteral(" · "));
		obs_properties_add_text(columns, "columns_shown", text.toUtf8().constData(), OBS_TEXT_INFO);
		obs_properties_add_button2(columns, "columns_edit", obs_module_text("Prop.ColumnsEdit"), editColumns,
					   source);
	}
	obs_properties_add_group(props, "columns", obs_module_text("Prop.Columns"), OBS_GROUP_NORMAL, columns);

	obs_properties_t *look = obs_properties_create();
	obs_properties_add_font(look, kFont, obs_module_text("Prop.Font"));
	obs_properties_add_color_alpha(look, kColorHeader, obs_module_text("Prop.ColorHeader"));
	obs_properties_add_color_alpha(look, kColorTotal, obs_module_text("Prop.ColorTotal"));
	obs_properties_add_int(look, kColumnSpacing, obs_module_text("Prop.ColumnSpacing"), 0, 200, 1);
	obs_properties_add_int(look, kRowSpacing, obs_module_text("Prop.RowSpacing"), 0, 200, 1);
	obs_properties_add_int_slider(look, kOpacity, obs_module_text("Prop.Opacity"), 0, 100, 1);
	obs_properties_add_group(props, "appearance", obs_module_text("Prop.Appearance"), OBS_GROUP_NORMAL, look);

	// The block every data row sits in.
	obs_properties_t *blocks = obs_properties_create();
	obs_properties_add_color(blocks, kBlockColor, obs_module_text("Prop.BlockColor"));
	obs_properties_add_int_slider(blocks, kBlockOpacity, obs_module_text("Prop.BlockOpacity"), 0, 100, 1);
	obs_properties_add_color_alpha(blocks, kBorderColor, obs_module_text("Prop.BorderColor"));
	obs_properties_add_int(blocks, kBorderWidth, obs_module_text("Prop.BorderWidth"), 0, 20, 1);
	obs_properties_add_int(blocks, kCornerRadius, obs_module_text("Prop.CornerRadius"), 0, 100, 1);
	obs_properties_add_int(blocks, kPaddingX, obs_module_text("Prop.PaddingX"), 0, 200, 1);
	obs_properties_add_int(blocks, kPaddingY, obs_module_text("Prop.PaddingY"), 0, 200, 1);
	obs_properties_add_group(props, "blocks", obs_module_text("Prop.Blocks"), OBS_GROUP_NORMAL, blocks);

	// A colour for each shown column, in the order they are shown.
	obs_properties_t *colors = obs_properties_create();
	if (p && source) {
		obs_data_t *settings = obs_source_get_settings(source);
		for (const QString &id : columnOrder(p->layout, settings)) {
			if (!obs_data_get_bool(settings, columnKey(id).constData()))
				continue;
			for (const Column &c : p->layout.columns())
				if (c.id == id)
					obs_properties_add_color_alpha(colors, colorKey(id).constData(),
								       columnLabel(c).toUtf8().constData());
		}
		obs_data_release(settings);
		obs_properties_add_button2(colors, "ranges_edit", obs_module_text("Prop.RangesEdit"), editRanges,
					   source);
	}
	obs_properties_add_group(props, "data_colors", obs_module_text("Prop.DataColors"), OBS_GROUP_NORMAL, colors);

	return props;
}

uint32_t getWidth(void *data)
{
	return (*holder(data))->cx.load();
}

uint32_t getHeight(void *data)
{
	return (*holder(data))->cy.load();
}

void videoRender(void *data, gs_effect_t *effect)
{
	OverlaySource &s = **holder(data);

	QImage image;
	bool changed = false;
	{
		std::lock_guard<std::mutex> lock(s.mu);
		if (s.hasPending) {
			image = std::move(s.pending);
			s.pending = QImage();
			s.hasPending = false;
			changed = true;
		}
	}
	if (changed) {
		if (image.isNull()) {
			if (s.texture)
				gs_texture_destroy(s.texture);
			s.texture = nullptr;
		} else {
			const auto w = static_cast<uint32_t>(image.width());
			const auto h = static_cast<uint32_t>(image.height());
			const uint8_t *bits = image.constBits();
			if (s.texture &&
			    (gs_texture_get_width(s.texture) != w || gs_texture_get_height(s.texture) != h)) {
				gs_texture_destroy(s.texture);
				s.texture = nullptr;
			}
			if (s.texture)
				gs_texture_set_image(s.texture, bits, static_cast<uint32_t>(image.bytesPerLine()),
						     false);
			else
				s.texture = gs_texture_create(w, h, GS_RGBA, 1, &bits, GS_DYNAMIC);
		}
	}
	if (!s.texture)
		return;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	gs_eparam_t *const param = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture_srgb(param, s.texture);
	gs_draw_sprite(s.texture, 0, gs_texture_get_width(s.texture), gs_texture_get_height(s.texture));
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);
}

} // namespace

void registerOverlaySource()
{
	obs_source_info info = {};
	info.id = kSourceId;
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	info.get_name = getName;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_defaults = getDefaults;
	info.get_properties = getProperties;
	info.get_width = getWidth;
	info.get_height = getHeight;
	info.video_render = videoRender;
	obs_register_source(&info);
}

namespace {

// A scene item of one of our sources, found while enumerating scenes.
struct ScaledItem {
	obs_sceneitem_t *item; // referenced
	std::shared_ptr<OverlaySource> source;
};

struct ScaleScan {
	std::vector<std::shared_ptr<OverlaySource>> sources;
	std::vector<ScaledItem> found;
};

bool collectItem(obs_scene_t *, obs_sceneitem_t *item, void *param)
{
	auto &scan = *static_cast<ScaleScan *>(param);
	if (obs_sceneitem_is_group(item))
		obs_sceneitem_group_enum_items(item, collectItem, param);
	if (obs_sceneitem_get_bounds_type(item) != OBS_BOUNDS_NONE)
		return true; // a bounding box is the streamer's explicit choice to scale
	obs_source_t *source = obs_sceneitem_get_source(item);
	for (const auto &s : scan.sources) {
		if (s->source == source) {
			obs_sceneitem_addref(item);
			scan.found.push_back({item, s});
			break;
		}
	}
	return true;
}

struct PendingScale {
	vec2 scale;
	qint64 since;
};

} // namespace

void normalizeSceneScales()
{
	ScaleScan scan;
	{
		std::lock_guard<std::mutex> lock(g_registryMu);
		for (const auto &w : g_registry)
			if (auto s = w.lock())
				scan.sources.push_back(std::move(s));
	}
	if (scan.sources.empty())
		return;
	obs_enum_scenes(
		[](void *param, obs_source_t *scene) {
			obs_scene_enum_items(obs_scene_from_source(scene), collectItem, param);
			return true;
		},
		&scan);

	// A scale held still for this long is a finished drag, not one in progress.
	constexpr qint64 kSettleMs = 400;
	static QHash<quintptr, PendingScale> pending;
	QSet<quintptr> seen;
	const qint64 now = QDateTime::currentMSecsSinceEpoch();

	for (const ScaledItem &f : scan.found) {
		const auto key = reinterpret_cast<quintptr>(f.item);
		seen.insert(key);
		vec2 scale;
		obs_sceneitem_get_scale(f.item, &scale);
		const bool unscaled = std::fabs(scale.x - 1.0f) < 0.001f && std::fabs(scale.y - 1.0f) < 0.001f;
		if (unscaled || scale.x <= 0.0f || scale.y <= 0.0f) { // flipped: leave it be
			pending.remove(key);
			continue;
		}
		auto it = pending.find(key);
		if (it == pending.end() || it->scale.x != scale.x || it->scale.y != scale.y) {
			pending.insert(key, {scale, now});
			continue;
		}
		if (now - it->since < kSettleMs)
			continue;

		// Resizing the source in the scene means a wider table, not a larger
		// font: the new width becomes the setting and the scale goes back to
		// 1. The height follows the rows, so a vertical stretch is undone.
		const uint32_t cx = f.source->cx.load();
		if (cx > 0) {
			obs_data_t *settings = obs_source_get_settings(f.source->source);
			obs_data_set_int(settings, kWidth, std::lround(cx * scale.x));
			obs_source_update(f.source->source, settings);
			obs_data_release(settings);
		}
		vec2 one;
		vec2_set(&one, 1.0f, 1.0f);
		obs_sceneitem_set_scale(f.item, &one);
		pending.remove(key);
	}
	for (auto it = pending.begin(); it != pending.end();)
		it = seen.contains(it.key()) ? std::next(it) : pending.erase(it);
	for (const ScaledItem &f : scan.found)
		obs_sceneitem_release(f.item);
}

void renderAllSources()
{
	std::vector<std::shared_ptr<OverlaySource>> sources;
	{
		std::lock_guard<std::mutex> lock(g_registryMu);
		for (const auto &w : g_registry)
			if (auto s = w.lock())
				sources.push_back(std::move(s));
	}
	for (const auto &s : sources)
		renderSource(*s);
}

} // namespace bss
