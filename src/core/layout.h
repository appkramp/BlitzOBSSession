#pragma once

// The statistics template: which columns exist, what each one computes and how
// it is formatted. Read from data/layout.json, so a column is added or a
// formula changed without touching the code — see docs/layout.md.

#include "session.h"

#include <QList>
#include <QString>

#include <memory>

namespace bss {

// An arithmetic expression over counter names: numbers, identifiers,
// + - * /, unary minus and parentheses. Division by zero gives 0, as the
// application's statistics do; an unknown counter is 0.
class Expression {
public:
	static std::shared_ptr<const Expression> parse(const QString &text, QString *error);
	virtual ~Expression() = default;
	virtual double eval(const Counters &c) const = 0;
};

struct Column {
	enum class Kind { ClassIcon, Tier, Name, Value };

	QString id;
	Kind kind = Kind::Value;
	QString header; // locale key of the column header; empty for none
	QString label;  // locale key naming the column in the settings
	std::shared_ptr<const Expression> expr;
	int decimals = 0;
	QString suffix;
	bool visibleByDefault = true;
};

class Layout {
public:
	static bool fromJson(const QByteArray &json, Layout &out, QString *error);

	const QList<Column> &columns() const { return columns_; }

private:
	QList<Column> columns_;
};

// "VIII" for 8; tiers are 1–10, anything else is written in digits.
QString romanTier(int tier);

} // namespace bss
