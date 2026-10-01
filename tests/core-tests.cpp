// Tests for src/core: the aggregation, the template's formulas, the catalogue
// and the protocol. No OBS, no network; Qt Core only.
//
//   cmake --build build_macos --config RelWithDebInfo --target bss-core-tests
//   build_macos/RelWithDebInfo/bss-core-tests

#include "core/catalog.h"
#include "core/layout.h"
#include "core/protocol.h"
#include "core/session.h"
#include "core/table.h"

#include <QFile>
#include <QJsonDocument>
#include <QTimeZone>

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace bss;

namespace {

int failures = 0;

#define CHECK(cond)                                                              \
	do {                                                                     \
		if (!(cond)) {                                                   \
			std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++failures;                                              \
		}                                                                \
	} while (0)

bool near(double a, double b)
{
	return std::fabs(a - b) < 1e-9;
}

BattleRecord record(qint64 id, int tank, const char *date, const QJsonObject &all)
{
	BattleRecord r;
	const QJsonObject obj{{"id", static_cast<double>(id)},
			      {"tank_id", tank},
			      {"date", QString::fromLatin1(date)},
			      {"count", 1},
			      {"approximate", false},
			      {"values", QJsonObject{{"all", all}, {"frags", 99}}}};
	BattleRecord::fromJson(obj, r);
	return r;
}

QJsonObject battle(int wins, int damage, int hits, int shots, int survived, int frags)
{
	return {{"battles", 1},     {"wins", wins},         {"damage_dealt", damage},
		{"hits", hits},     {"shots", shots},       {"survived_battles", survived},
		{"frags", frags},   {"nickname", "ignored"}};
}

void testSessionSumsAndDeduplicates()
{
	Session s;
	CHECK(s.add(record(10, 3089, "2026-09-29T17:00:00Z", battle(1, 2000, 7, 9, 1, 2))));
	CHECK(s.add(record(11, 3089, "2026-09-29T17:10:00Z", battle(0, 1000, 3, 9, 0, 0))));
	CHECK(s.add(record(12, 49, "2026-09-29T17:20:00Z", battle(1, 3000, 5, 5, 1, 3))));
	// A resumed connection may send a record again; it counts once.
	CHECK(!s.add(record(11, 3089, "2026-09-29T17:10:00Z", battle(0, 1000, 3, 9, 0, 0))));

	CHECK(s.lastId() == 12);
	CHECK(near(s.totals().value("battles"), 3));
	CHECK(near(s.totals().value("damage_dealt"), 6000));
	CHECK(near(s.tanks().value(3089).counters.value("battles"), 2));
	CHECK(near(s.tanks().value(3089).counters.value("hits"), 10));
	// Only `values.all` is read; a sibling field of the same name is not.
	CHECK(near(s.totals().value("frags"), 5));
	// Strings are not counters.
	CHECK(!s.totals().values().contains("nickname"));

	const auto recent = s.ordered(Session::Order::LastPlayed);
	CHECK(recent.size() == 2 && recent[0].tankId == 49);
	const auto most = s.ordered(Session::Order::Battles);
	CHECK(most.size() == 2 && most[0].tankId == 3089);

	s.clear();
	CHECK(s.isEmpty() && s.lastId() == 0 && s.totals().isEmpty());
}

void testRecordRejectsMissingIds()
{
	BattleRecord r;
	CHECK(!BattleRecord::fromJson(QJsonObject{{"tank_id", 1}}, r));
	CHECK(!BattleRecord::fromJson(QJsonObject{{"id", 1}}, r));
}

double evalExpr(const char *text, const Counters &c)
{
	QString error;
	auto e = Expression::parse(QString::fromLatin1(text), &error);
	if (!e) {
		std::printf("  parse error for %s: %s\n", text, qPrintable(error));
		++failures;
		return NAN;
	}
	return e->eval(c);
}

void testExpressions()
{
	Counters c;
	c.add(QJsonObject{{"battles", 4}, {"wins", 3}, {"hits", 0}, {"shots", 0}});

	CHECK(near(evalExpr("wins / battles * 100", c), 75));
	CHECK(near(evalExpr("1 + 2 * 3", c), 7));
	CHECK(near(evalExpr("(1 + 2) * 3", c), 9));
	CHECK(near(evalExpr("-wins + 10", c), 7));
	CHECK(near(evalExpr("battles - wins - 1", c), 0));
	CHECK(near(evalExpr("0.5 * battles", c), 2));
	// As in the application: nothing to divide by is 0, not NaN.
	CHECK(near(evalExpr("hits / shots * 100", c), 0));
	CHECK(near(evalExpr("unknown_counter + 1", c), 1));

	QString error;
	CHECK(!Expression::parse("wins /", &error) && !error.isEmpty());
	CHECK(!Expression::parse("(wins", &error));
	CHECK(!Expression::parse("wins battles", &error));
	CHECK(!Expression::parse("", &error));
}

void testLayoutFile()
{
	QFile f(QStringLiteral(BSS_SOURCE_DIR "/data/layout.json"));
	CHECK(f.open(QIODevice::ReadOnly));
	Layout layout;
	QString error;
	CHECK(Layout::fromJson(f.readAll(), layout, &error));
	if (!error.isEmpty())
		std::printf("  layout.json: %s\n", qPrintable(error));

	QStringList ids;
	for (const Column &c : layout.columns())
		ids << c.id;
	for (const char *id : {"class", "tier", "name", "battles", "winrate", "damage", "accuracy", "survival", "frags"})
		CHECK(ids.contains(QString::fromLatin1(id)));

	// The shipped formulas give the application's numbers.
	Counters c;
	c.add(battle(1, 2000, 7, 10, 1, 2));
	c.add(battle(0, 1000, 3, 10, 0, 1));
	auto value = [&](const char *id) -> double {
		for (const Column &col : layout.columns())
			if (col.id == QLatin1String(id))
				return col.expr->eval(c);
		return NAN;
	};
	CHECK(near(value("battles"), 2));
	CHECK(near(value("winrate"), 50));
	CHECK(near(value("damage"), 1500));
	CHECK(near(value("accuracy"), 50));
	CHECK(near(value("survival"), 50));
	CHECK(near(value("frags"), 1.5));
}

void testLayoutErrors()
{
	Layout l;
	QString error;
	CHECK(!Layout::fromJson("{}", l, &error));
	CHECK(!Layout::fromJson(R"({"version":1,"columns":[{"id":"a","expr":"1 +"}]})", l, &error));
	CHECK(!Layout::fromJson(R"({"version":1,"columns":[{"id":"a","expr":"1"},{"id":"a","expr":"2"}]})", l, &error));
	CHECK(!Layout::fromJson(R"({"version":1,"columns":[{"id":"a","kind":"sparkline"}]})", l, &error));
	CHECK(Layout::fromJson(R"({"version":1,"columns":[{"id":"a","expr":"xp / battles"}]})", l, &error));
}

void testRoman()
{
	CHECK(romanTier(1) == "I");
	CHECK(romanTier(4) == "IV");
	CHECK(romanTier(8) == "VIII");
	CHECK(romanTier(9) == "IX");
	CHECK(romanTier(10) == "X");
	CHECK(romanTier(0).isEmpty());
	CHECK(romanTier(11) == "11");
}

void testCatalog()
{
	const QByteArray manifest = R"({"schema_version":1,"revision":3,
		"catalog":{"url":"https://cdn.appkramp.com/content/data/vehicles.v1.json",
		"sha256":"104F8D98497279B12CA9219AB89C1A2384CE9A9120DAEAC3B13DA37E5292A5C1"}})";
	CatalogPointer p;
	CHECK(Catalog::parseManifest(manifest, p));
	CHECK(p.revision == 3 && p.sha256.startsWith("104f8d"));
	CHECK(!Catalog::parseManifest(R"({"schema_version":2})", p));

	const QByteArray json = R"({"schema_version":1,"revision":1,"vehicles":{
		"1":{"tier":5,"type":"mediumTank","names":{"en":"T-34","ru":"Т-34","uk":"T-34"}},
		"49":{"tier":8,"type":"mediumTank","names":{"ru":"Type 59"}}}})";
	Catalog cat;
	CHECK(Catalog::parse(json, cat));
	CHECK(cat.size() == 2);
	CHECK(cat.find(1) && cat.find(1)->name("ru") == QString::fromUtf8("Т-34"));
	CHECK(cat.find(1)->name("pl") == "T-34");
	CHECK(cat.find(49)->name("en") == "Type 59");
	CHECK(cat.find(49)->tier == 8);
	CHECK(cat.find(2) == nullptr);
}

void testProtocol()
{
	const QJsonObject hello = QJsonDocument::fromJson(protocol::hello("ovk_x", "test")).object();
	CHECK(hello.value("type") == "hello" && hello.value("protocol") == 1 && hello.value("key") == "ovk_x");

	const QDateTime since(QDate(2026, 9, 29), QTime(0, 0), QTimeZone(3 * 3600));
	const QJsonObject sub = QJsonDocument::fromJson(protocol::subscribe("eu", since, 42)).object();
	CHECK(sub.value("since") == "2026-09-28T21:00:00Z");
	CHECK(sub.value("after_id").toInteger() == 42);
	CHECK(sub.value("realm") == "eu");

	protocol::Message m;
	CHECK(protocol::parse(R"({"type":"welcome","protocol":1,"account":{"account_id":7,"nickname":"N","realms":["eu","com"]}})", m));
	CHECK(m.type == protocol::Message::Type::Welcome && m.account.accountId == 7);
	CHECK(m.account.realms == QStringList({"eu", "com"}));

	CHECK(protocol::parse(R"({"type":"history","realm":"eu","has_more":true,"battles":[
		{"id":5,"tank_id":1,"date":"2026-09-29T10:00:00Z","count":1,"values":{"all":{"battles":1}}},
		{"tank_id":1}]})", m));
	CHECK(m.type == protocol::Message::Type::History && m.hasMore && m.battles.size() == 1);
	CHECK(m.battles[0].date == QDateTime(QDate(2026, 9, 29), QTime(10, 0), QTimeZone::UTC));

	CHECK(protocol::parse(R"({"type":"error","code":"key_invalid","fatal":true})", m));
	CHECK(m.type == protocol::Message::Type::Error && m.fatal && m.errorCode == "key_invalid");

	CHECK(protocol::parse(R"({"type":"something_new"})", m));
	CHECK(m.type == protocol::Message::Type::Unknown);
	CHECK(!protocol::parse("not json", m));
	CHECK(!protocol::parse(R"({"no":"type"})", m));
}

void testKeysAndBackoff()
{
	CHECK(protocol::looksLikeKey("ovk_0123456789abcdefghijABCDEFGHIJ-_"));
	CHECK(!protocol::looksLikeKey("ovk_short"));
	CHECK(!protocol::looksLikeKey(" ovk_0123456789abcdefghijABCDEFGHIJ-_"));

	CHECK(protocol::reconnectDelayMs(0, 0) == 1000);
	CHECK(protocol::reconnectDelayMs(3, 0) == 8000);
	CHECK(protocol::reconnectDelayMs(20, 0) == 60000);
	CHECK(protocol::reconnectDelayMs(20, 1) == 72000);
}

void testStartOfLocalDay()
{
	const QDateTime now = QDateTime(QDate(2026, 9, 29), QTime(23, 59, 30));
	const QDateTime start = protocol::startOfLocalDay(now);
	CHECK(start.date() == QDate(2026, 9, 29) && start.time() == QTime(0, 0));
}


Layout shippedLayout()
{
	QFile f(QStringLiteral(BSS_SOURCE_DIR "/data/layout.json"));
	f.open(QIODevice::ReadOnly);
	Layout layout;
	Layout::fromJson(f.readAll(), layout, nullptr);
	return layout;
}

void testTable()
{
	const Layout layout = shippedLayout();
	Catalog cat;
	Catalog::parse(R"({"schema_version":1,"revision":1,"vehicles":{
		"3089":{"tier":8,"type":"heavyTank","names":{"en":"Heavy","ru":"Тяж"}},
		"49":{"tier":10,"type":"mediumTank","names":{"en":"Medium"}}}})", cat);
	Session s;
	s.add(record(1, 3089, "2026-09-29T17:00:00Z", battle(1, 2000, 7, 10, 1, 2)));
	s.add(record(2, 3089, "2026-09-29T17:10:00Z", battle(0, 1001, 3, 10, 0, 1)));
	s.add(record(3, 49, "2026-09-29T17:20:00Z", battle(1, 3000, 5, 5, 1, 3)));
	s.add(record(4, 7777, "2026-09-29T17:30:00Z", battle(0, 100, 0, 0, 0, 0)));

	TableOptions o;
	o.columns = {"class", "tier", "name", "battles", "winrate", "damage", "accuracy"};
	o.language = "ru";
	auto tr = [](const QString &key) { return QStringLiteral("<%1>").arg(key); };

	Table t = buildTable(layout, s, cat, o, tr);
	CHECK(t.columns == 7);
	CHECK(t.columnIds == QStringList({"class", "tier", "name", "battles", "winrate", "damage", "accuracy"}));
	CHECK(t.identity == QList<bool>({true, true, true, false, false, false, false}));
	CHECK(t.header.size() == 7 && t.header[1].text == "<Overlay.Col.Tier>" && t.header[0].text.isEmpty());
	CHECK(t.rows.size() == 3);
	// Most recent first; an unknown vehicle still gets a row.
	CHECK(t.rows[0][2].text == "#7777" && t.rows[0][0].text.isEmpty() && t.rows[0][1].text.isEmpty());
	CHECK(t.rows[1][2].text == "Medium" && t.rows[1][1].text == "X" && t.rows[1][0].text == "mediumTank");
	CHECK(t.rows[2][2].text == QString::fromUtf8("Тяж") && t.rows[2][1].text == "VIII");
	// Russian decimal comma, no thousands separator.
	CHECK(t.rows[2][3].text == "2" && t.rows[2][4].text == QString::fromUtf8("50,0%"));
	CHECK(t.rows[2][5].text == "1500" || t.rows[2][5].text == "1501"); // 1500.5 rounds either way
	CHECK(t.rows[0][6].text == QString::fromUtf8("0,0%"));             // no shots: 0, as in the app
	CHECK(t.total.size() == 7 && t.total[2].text == "<Overlay.Total>" && t.total[3].text == "4");
	CHECK(t.total[4].text == QString::fromUtf8("50,0%"));

	o.language = "en";
	o.rows = TableOptions::Rows::Selected;
	o.selectedTanks = {49};
	o.showHeader = false;
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.header.isEmpty() && t.rows.size() == 1 && t.rows[0][2].text == "Medium");
	CHECK(t.rows[0][4].text == "100.0%");

	o.rows = TableOptions::Rows::TotalsOnly;
	o.showTotal = false; // totals-only shows the total row regardless
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows.isEmpty() && !t.total.isEmpty());

	o.rows = TableOptions::Rows::All;
	o.showTotal = true;
	o.maxRows = 2;
	o.sortBy = "battles";
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows.size() == 2 && t.rows[0][2].text == "Heavy");
	o.maxRows = 0;

	// Any column sorts, either way; ties keep the most recent first.
	o.sortBy = "damage";
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows.size() == 3 && t.rows[0][2].text == "Medium" && t.rows[2][2].text == "#7777");
	o.ascending = true;
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows[0][2].text == "#7777" && t.rows[2][2].text == "Medium");
	o.sortBy = "tier";
	o.ascending = false;
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows[0][1].text == "X" && t.rows[1][1].text == "VIII" && t.rows[2][2].text == "#7777");
	o.sortBy = "name";
	o.ascending = true;
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows[0][2].text == "#7777" && t.rows[1][2].text == "Heavy" && t.rows[2][2].text == "Medium");
	o.sortBy = "no_such_column"; // falls back to the most recent first
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows[0][2].text == "#7777");
	o.sortBy.clear();
	o.ascending = false;

	// Columns come in the order given, not the layout's.
	o.showHeader = true;
	o.columns = {"battles", "name", "tier"};
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows[0].size() == 3 && t.rows[0][0].text == "1" && t.rows[0][1].text == "#7777");
	CHECK(t.header[0].text == "<Overlay.Col.Battles>");
	CHECK(t.total[1].text == "<Overlay.Total>" && t.total[0].text == "4");

	// "Total" moves to the last column before the values when the name is hidden.
	o.columns = {"class", "tier", "battles"};
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.total[1].text == "<Overlay.Total>" && t.total[0].text.isEmpty());

	// No battles yet: dashes, not zeros that look like results.
	Session empty;
	o.columns = {"name", "winrate"};
	t = buildTable(layout, empty, cat, o, tr);
	CHECK(t.rows.isEmpty() && t.total[1].text == QString::fromUtf8("\u2014"));
}

void testGrouping()
{
	const Layout layout = shippedLayout();
	Catalog cat;
	Catalog::parse(R"({"schema_version":1,"revision":1,"vehicles":{
		"1":{"tier":8,"type":"heavyTank","names":{"en":"H8"}},
		"2":{"tier":8,"type":"mediumTank","names":{"en":"M8"}},
		"3":{"tier":10,"type":"heavyTank","names":{"en":"H10"}},
		"4":{"tier":8,"type":"heavyTank","names":{"en":"H8b"}}}})", cat);
	Session s;
	s.add(record(1, 1, "2026-09-29T17:00:00Z", battle(1, 2000, 1, 1, 1, 1)));
	s.add(record(2, 2, "2026-09-29T17:10:00Z", battle(0, 1000, 1, 1, 0, 0)));
	s.add(record(3, 3, "2026-09-29T17:20:00Z", battle(1, 3000, 1, 1, 1, 2)));
	s.add(record(4, 4, "2026-09-29T17:30:00Z", battle(1, 1000, 1, 1, 0, 1)));
	s.add(record(5, 1, "2026-09-29T17:40:00Z", battle(0, 3000, 1, 1, 0, 0)));
	auto tr = [](const QString &key) { return key; };

	TableOptions o;
	o.language = "en";
	o.showHeader = false;
	o.showTotal = false;

	// By tier: VIII (four battles, three vehicles), then X; the most recent first.
	o.columns = {"tier", "battles", "damage"};
	Table t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows.size() == 2);
	CHECK(t.rows[0][0].text == "VIII" && t.rows[0][1].text == "4" && t.rows[0][2].text == "1750");
	CHECK(t.rows[1][0].text == "X" && t.rows[1][1].text == "1");

	// By class: heavy (four battles over tiers VIII and X), then medium.
	o.columns = {"class", "battles"};
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows.size() == 2);
	CHECK(t.rows[0][0].kind == Cell::Kind::ClassIcon && t.rows[0][0].text == "heavyTank" && t.rows[0][1].text == "4");
	CHECK(t.rows[1][0].text == "mediumTank" && t.rows[1][1].text == "1");

	// By class and tier together.
	o.columns = {"class", "tier", "battles"};
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows.size() == 3);
	CHECK(t.rows[0][0].text == "heavyTank" && t.rows[0][1].text == "VIII" && t.rows[0][2].text == "3");

	// The name brings back one row per vehicle.
	o.columns = {"tier", "name", "battles"};
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows.size() == 4);

	// Sorting applies to the groups; selection filters before grouping.
	o.columns = {"tier", "battles"};
	o.sortBy = "tier";
	o.ascending = true;
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows[0][0].text == "VIII" && t.rows[1][0].text == "X");
	o.sortBy.clear();
	o.ascending = false;
	o.rows = TableOptions::Rows::Selected;
	o.selectedTanks = {1, 3};
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows.size() == 2 && t.rows[0][0].text == "VIII" && t.rows[0][1].text == "2");
	o.rows = TableOptions::Rows::All;

	// Nothing tells rows apart: only the total remains.
	o.columns = {"battles", "winrate"};
	o.showTotal = true;
	t = buildTable(layout, s, cat, o, tr);
	CHECK(t.rows.isEmpty() && t.total.size() == 2 && t.total[0].text == "5");
}

void testColumnOrder()
{
	const Layout layout = shippedLayout();
	const QStringList all = orderedColumnIds(layout, {});
	CHECK(all.size() == layout.columns().size() && all.first() == "class");

	const QStringList saved = orderedColumnIds(layout, {"damage", "gone", "name", "damage"});
	CHECK(saved.size() == all.size());
	CHECK(saved[0] == "damage" && saved[1] == "name" && saved[2] == "class");
	CHECK(!saved.contains("gone"));
}

} // namespace

int main()
{
	const std::vector<std::pair<const char *, std::function<void()>>> tests = {
		{"session sums and deduplicates", testSessionSumsAndDeduplicates},
		{"record rejects missing ids", testRecordRejectsMissingIds},
		{"expressions", testExpressions},
		{"layout.json", testLayoutFile},
		{"layout errors", testLayoutErrors},
		{"roman tiers", testRoman},
		{"catalogue", testCatalog},
		{"protocol", testProtocol},
		{"keys and backoff", testKeysAndBackoff},
		{"start of local day", testStartOfLocalDay},
		{"table", testTable},
		{"grouping", testGrouping},
		{"column order", testColumnOrder},
	};
	for (const auto &[name, fn] : tests) {
		const int before = failures;
		fn();
		std::printf("%s %s\n", failures == before ? "ok  " : "FAIL", name);
	}
	std::printf(failures ? "\n%d check(s) failed\n" : "\nall passed\n", failures);
	return failures ? 1 : 0;
}
