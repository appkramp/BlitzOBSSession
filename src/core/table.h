#pragma once

// What one overlay shows: the rows, the cells, their text — built from the
// session, the catalogue and the source's options. The renderer only paints
// what this produces.

#include "catalog.h"
#include "layout.h"
#include "session.h"

#include <QList>
#include <QLocale>
#include <QSet>
#include <QString>

#include <functional>
#include <optional>

namespace bss {

struct Cell {
	enum class Kind { Text, ClassIcon };
	enum class Align { Left, Center, Right };

	Kind kind = Kind::Text;
	Align align = Align::Right;
	QString text; // Text: the text; ClassIcon: the vehicle type, empty for none
	// A number's value as shown (rounded to its decimals), for colours that
	// depend on it; absent for text and for a dash.
	std::optional<double> value;
};

struct Table {
	QList<Cell> header; // empty when the header is off
	QList<QList<Cell>> rows;
	QList<Cell> total; // empty when the total row is off
	int columns = 0;
	QStringList columnIds; // the layout id of each column, in display order
	// Per column: it names the row (class, tier, name) rather than measuring it.
	QList<bool> identity;
	// Per column: the widest text it is expected to hold, for a table of fixed
	// size — "8888", "888,8%", "VIII"; empty for the icon and the name.
	QStringList samples;
	// Per column: it takes the width left over in a table of fixed size (the
	// name).
	QList<bool> stretch;
};

struct TableOptions {
	enum class Rows { All, Selected, TotalsOnly };

	Rows rows = Rows::All;
	QString sortBy;          // a column id; empty: the most recently played first
	bool ascending = false;  // for sortBy
	int maxRows = 0;         // 0: no limit
	QSet<int> selectedTanks; // for Rows::Selected
	QStringList columns;     // ids of the visible columns, in display order
	bool showHeader = true;
	bool showTotal = true;
	QString language = QStringLiteral("en"); // ru, en, uk
};

// Looks a locale key up in the overlay's language.
using Translate = std::function<QString(const QString &key)>;

Table buildTable(const Layout &layout, const Session &session, const Catalog &catalog, const TableOptions &options,
		 const Translate &tr);

// Every column id of the layout in the order a source saved, the columns the
// saved order does not know (added to layout.json since) at the end in the
// layout's order, ids the layout no longer has dropped.
QStringList orderedColumnIds(const Layout &layout, const QStringList &saved);

// A value as the overlay writes it: the language's decimal separator, no
// thousands separator, and a dash when the row has no battles.
QString formatValue(double value, int decimals, const QString &suffix, const QLocale &locale);

} // namespace bss
