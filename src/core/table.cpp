#include "table.h"

#include <QHash>

#include <algorithm>

namespace bss {

namespace {

QLocale localeFor(const QString &language)
{
	if (language == QLatin1String("ru"))
		return QLocale(QLocale::Russian, QLocale::Russia);
	if (language == QLatin1String("uk"))
		return QLocale(QLocale::Ukrainian, QLocale::Ukraine);
	return QLocale(QLocale::English, QLocale::UnitedStates);
}

const QString kDash = QStringLiteral("—");

const Column *findColumn(const Layout &layout, const QString &id)
{
	for (const Column &c : layout.columns())
		if (c.id == id)
			return &c;
	return nullptr;
}

int classRank(const QString &type)
{
	static const QStringList order = {QStringLiteral("lightTank"), QStringLiteral("mediumTank"),
					  QStringLiteral("heavyTank"), QStringLiteral("AT-SPG")};
	const qsizetype i = order.indexOf(type);
	return static_cast<int>(i < 0 ? order.size() : i);
}

template<typename T> int threeWay(const T &x, const T &y)
{
	return x < y ? -1 : y < x ? 1 : 0;
}

// One row of the table: a vehicle, or a group of vehicles sharing a tier, a
// class, or both.
struct RowData {
	int tankId = 0; // 0 for a group
	int tier = 0;
	QString type;
	Counters counters;
	QDateTime lastBattle;
};

// What a row stands for follows from the columns that tell rows apart: with
// the tank's name, one row per vehicle; without it, one per tier, per class,
// or per class and tier — whichever of those columns is shown. With none of
// them the rows could not be told apart, so there are none, only the total.
enum class Grouping { Vehicle, Tier, Class, ClassAndTier, None };

Grouping groupingFor(const QList<const Column *> &columns)
{
	bool name = false, tier = false, cls = false;
	for (const Column *c : columns) {
		name = name || c->kind == Column::Kind::Name;
		tier = tier || c->kind == Column::Kind::Tier;
		cls = cls || c->kind == Column::Kind::ClassIcon;
	}
	if (name)
		return Grouping::Vehicle;
	if (tier && cls)
		return Grouping::ClassAndTier;
	if (tier)
		return Grouping::Tier;
	if (cls)
		return Grouping::Class;
	return Grouping::None;
}

QList<RowData> collectRows(const Session &session, const Catalog &catalog, const TableOptions &options,
			   Grouping grouping)
{
	QList<RowData> rows;
	if (grouping == Grouping::None)
		return rows;

	QHash<QString, qsizetype> groups;
	for (const TankTotals &tank : session.tanks()) {
		if (options.rows == TableOptions::Rows::Selected && !options.selectedTanks.contains(tank.tankId))
			continue;
		const Vehicle *v = catalog.find(tank.tankId);
		RowData r;
		r.tankId = tank.tankId;
		r.tier = v ? v->tier : 0;
		r.type = v ? v->type : QString();
		r.counters = tank.counters;
		r.lastBattle = tank.lastBattle;
		if (grouping == Grouping::Vehicle) {
			rows.append(r);
			continue;
		}

		r.tankId = 0;
		if (grouping == Grouping::Tier)
			r.type.clear();
		if (grouping == Grouping::Class)
			r.tier = 0;
		const QString key = r.type + QLatin1Char('|') + QString::number(r.tier);
		auto it = groups.constFind(key);
		if (it == groups.constEnd()) {
			groups.insert(key, rows.size());
			rows.append(r);
			continue;
		}
		RowData &g = rows[it.value()];
		g.counters.add(r.counters);
		if (r.lastBattle > g.lastBattle)
			g.lastBattle = r.lastBattle;
	}

	// Most recently played first; the rest only makes the order stable.
	std::sort(rows.begin(), rows.end(), [](const RowData &a, const RowData &b) {
		if (a.lastBattle != b.lastBattle)
			return a.lastBattle > b.lastBattle;
		if (a.tier != b.tier)
			return a.tier > b.tier;
		if (a.type != b.type)
			return classRank(a.type) < classRank(b.type);
		return a.tankId < b.tankId;
	});
	return rows;
}

QString rowName(const RowData &r, const Catalog &catalog, const QString &language)
{
	if (r.tankId == 0)
		return QString();
	const Vehicle *v = catalog.find(r.tankId);
	return v ? v->name(language) : QStringLiteral("#%1").arg(r.tankId);
}

// Stably re-sorted by the chosen column, so rows equal in it keep the order
// they were last played in.
void sortRows(QList<RowData> &rows, const Layout &layout, const Catalog &catalog, const TableOptions &options)
{
	const Column *by = options.sortBy.isEmpty() ? nullptr : findColumn(layout, options.sortBy);
	if (!by)
		return;

	// Negative, zero or positive as a sorts before, with or after b, ascending.
	auto compare = [&](const RowData &a, const RowData &b) -> int {
		switch (by->kind) {
		case Column::Kind::Value:
			return threeWay(by->expr->eval(a.counters), by->expr->eval(b.counters));
		case Column::Kind::Tier:
			return threeWay(a.tier, b.tier);
		case Column::Kind::ClassIcon:
			return threeWay(classRank(a.type), classRank(b.type));
		case Column::Kind::Name:
			return rowName(a, catalog, options.language)
				.localeAwareCompare(rowName(b, catalog, options.language));
		}
		return 0;
	};
	std::stable_sort(rows.begin(), rows.end(), [&](const RowData &a, const RowData &b) {
		const int c = compare(a, b);
		return options.ascending ? c < 0 : c > 0;
	});
}

} // namespace

QStringList orderedColumnIds(const Layout &layout, const QStringList &saved)
{
	QStringList ids;
	for (const QString &id : saved)
		if (findColumn(layout, id) && !ids.contains(id))
			ids.append(id);
	for (const Column &c : layout.columns())
		if (!ids.contains(c.id))
			ids.append(c.id);
	return ids;
}

QString formatValue(double value, int decimals, const QString &suffix, const QLocale &locale)
{
	QLocale l(locale);
	l.setNumberOptions(QLocale::OmitGroupSeparator);
	return l.toString(value, 'f', decimals) + suffix;
}

Table buildTable(const Layout &layout, const Session &session, const Catalog &catalog, const TableOptions &options,
		 const Translate &tr)
{
	const QLocale locale = localeFor(options.language);

	QList<const Column *> columns;
	for (const QString &id : options.columns)
		if (const Column *c = findColumn(layout, id))
			columns.append(c);

	Table table;
	table.columns = static_cast<int>(columns.size());
	for (const Column *c : columns) {
		table.columnIds.append(c->id);
		table.identity.append(c->kind != Column::Kind::Value);
		table.stretch.append(c->kind == Column::Kind::Name);
		QString sample;
		if (c->kind == Column::Kind::Tier) {
			sample = QStringLiteral("VIII");
		} else if (c->kind == Column::Kind::Value) {
			// Four digits cover battles and damage, three a percentage.
			sample = QString(c->suffix == QLatin1String("%") ? 3 : 4, QLatin1Char('8'));
			if (c->decimals > 0)
				sample += locale.decimalPoint() + QString(c->decimals, QLatin1Char('8'));
			sample += c->suffix;
		}
		table.samples.append(sample);
	}
	if (columns.isEmpty())
		return table;

	auto alignOf = [](const Column &c) {
		switch (c.kind) {
		case Column::Kind::Name:
			return Cell::Align::Left;
		case Column::Kind::ClassIcon:
		case Column::Kind::Tier:
			return Cell::Align::Center;
		case Column::Kind::Value:
			break;
		}
		return Cell::Align::Right;
	};

	auto valueCell = [&](const Column &c, const Counters &counters) {
		Cell cell;
		cell.align = Cell::Align::Right;
		cell.text = counters.value(QStringLiteral("battles")) > 0
				    ? formatValue(c.expr->eval(counters), c.decimals, c.suffix, locale)
				    : kDash;
		return cell;
	};

	auto dataRow = [&](const RowData &r) {
		QList<Cell> row;
		for (const Column *c : columns) {
			Cell cell;
			cell.align = alignOf(*c);
			switch (c->kind) {
			case Column::Kind::ClassIcon:
				cell.kind = Cell::Kind::ClassIcon;
				cell.text = r.type;
				break;
			case Column::Kind::Tier:
				cell.text = romanTier(r.tier);
				break;
			case Column::Kind::Name:
				cell.text = rowName(r, catalog, options.language);
				break;
			case Column::Kind::Value:
				cell = valueCell(*c, r.counters);
				break;
			}
			row.append(cell);
		}
		return row;
	};

	if (options.showHeader) {
		for (const Column *c : columns) {
			Cell cell;
			cell.align = alignOf(*c);
			cell.text = c->header.isEmpty() ? QString() : tr(c->header);
			table.header.append(cell);
		}
	}

	if (options.rows != TableOptions::Rows::TotalsOnly) {
		QList<RowData> rows = collectRows(session, catalog, options, groupingFor(columns));
		sortRows(rows, layout, catalog, options);
		for (const RowData &r : rows) {
			table.rows.append(dataRow(r));
			if (options.maxRows > 0 && table.rows.size() >= options.maxRows)
				break;
		}
	}

	if (options.showTotal || options.rows == TableOptions::Rows::TotalsOnly) {
		// "Total" goes in the name column wherever it is; with the name
		// hidden, in the last text column before the first value, or else the
		// first text column there is.
		qsizetype labelAt = -1;
		for (qsizetype i = 0; i < columns.size(); ++i)
			if (columns[i]->kind == Column::Kind::Name)
				labelAt = i;
		for (qsizetype i = 0; labelAt < 0 && i < columns.size() && columns[i]->kind != Column::Kind::Value; ++i)
			if (i + 1 == columns.size() || columns[i + 1]->kind == Column::Kind::Value)
				labelAt = i;
		for (qsizetype i = 0; labelAt < 0 && i < columns.size(); ++i)
			if (columns[i]->kind != Column::Kind::Value)
				labelAt = i;
		for (qsizetype i = 0; i < columns.size(); ++i) {
			const Column &c = *columns[i];
			Cell cell;
			cell.align = alignOf(c);
			if (c.kind == Column::Kind::Value) {
				cell = valueCell(c, session.totals());
			} else if (i == labelAt) {
				cell.text = tr(QStringLiteral("Overlay.Total"));
				cell.align = Cell::Align::Left;
			}
			table.total.append(cell);
		}
	}
	return table;
}

} // namespace bss
