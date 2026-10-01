#include "layout.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace bss {

namespace {

class Number : public Expression {
public:
	explicit Number(double v) : v_(v) {}
	double eval(const Counters &) const override { return v_; }

private:
	double v_;
};

class Counter : public Expression {
public:
	explicit Counter(QString name) : name_(std::move(name)) {}
	double eval(const Counters &c) const override { return c.value(name_); }

private:
	QString name_;
};

class Negate : public Expression {
public:
	explicit Negate(std::shared_ptr<const Expression> e) : e_(std::move(e)) {}
	double eval(const Counters &c) const override { return -e_->eval(c); }

private:
	std::shared_ptr<const Expression> e_;
};

class Binary : public Expression {
public:
	Binary(QChar op, std::shared_ptr<const Expression> l, std::shared_ptr<const Expression> r)
		: op_(op),
		  l_(std::move(l)),
		  r_(std::move(r))
	{
	}

	double eval(const Counters &c) const override
	{
		const double a = l_->eval(c);
		const double b = r_->eval(c);
		switch (op_.unicode()) {
		case '+':
			return a + b;
		case '-':
			return a - b;
		case '*':
			return a * b;
		default:
			return b == 0.0 ? 0.0 : a / b;
		}
	}

private:
	QChar op_;
	std::shared_ptr<const Expression> l_, r_;
};

// Recursive descent over:
//   expr   := term (('+' | '-') term)*
//   term   := factor (('*' | '/') factor)*
//   factor := number | identifier | '-' factor | '(' expr ')'
class Parser {
public:
	explicit Parser(const QString &s) : s_(s) {}

	std::shared_ptr<const Expression> parse(QString *error)
	{
		auto e = expr();
		skipSpace();
		if (e && pos_ != s_.size())
			fail(QStringLiteral("unexpected '%1'").arg(s_.mid(pos_, 1)));
		if (!error_.isEmpty()) {
			if (error)
				*error = QStringLiteral("%1 at %2").arg(error_).arg(pos_);
			return nullptr;
		}
		return e;
	}

private:
	void skipSpace()
	{
		while (pos_ < s_.size() && s_[pos_].isSpace())
			++pos_;
	}

	bool take(QChar c)
	{
		skipSpace();
		if (pos_ < s_.size() && s_[pos_] == c) {
			++pos_;
			return true;
		}
		return false;
	}

	void fail(const QString &message)
	{
		if (error_.isEmpty())
			error_ = message;
	}

	std::shared_ptr<const Expression> expr()
	{
		auto left = term();
		while (left) {
			QChar op;
			if (take(QLatin1Char('+')))
				op = QLatin1Char('+');
			else if (take(QLatin1Char('-')))
				op = QLatin1Char('-');
			else
				break;
			auto right = term();
			if (!right)
				return nullptr;
			left = std::make_shared<Binary>(op, left, right);
		}
		return left;
	}

	std::shared_ptr<const Expression> term()
	{
		auto left = factor();
		while (left) {
			QChar op;
			if (take(QLatin1Char('*')))
				op = QLatin1Char('*');
			else if (take(QLatin1Char('/')))
				op = QLatin1Char('/');
			else
				break;
			auto right = factor();
			if (!right)
				return nullptr;
			left = std::make_shared<Binary>(op, left, right);
		}
		return left;
	}

	std::shared_ptr<const Expression> factor()
	{
		if (take(QLatin1Char('-'))) {
			auto e = factor();
			return e ? std::make_shared<Negate>(e) : nullptr;
		}
		if (take(QLatin1Char('('))) {
			auto e = expr();
			if (e && !take(QLatin1Char(')'))) {
				fail(QStringLiteral("missing ')'"));
				return nullptr;
			}
			return e;
		}
		skipSpace();
		const qsizetype start = pos_;
		if (pos_ < s_.size() && (s_[pos_].isDigit() || s_[pos_] == QLatin1Char('.'))) {
			while (pos_ < s_.size() && (s_[pos_].isDigit() || s_[pos_] == QLatin1Char('.')))
				++pos_;
			bool ok = false;
			const double v = s_.mid(start, pos_ - start).toDouble(&ok);
			if (!ok) {
				fail(QStringLiteral("bad number"));
				return nullptr;
			}
			return std::make_shared<Number>(v);
		}
		if (pos_ < s_.size() && (s_[pos_].isLetter() || s_[pos_] == QLatin1Char('_'))) {
			while (pos_ < s_.size() && (s_[pos_].isLetterOrNumber() || s_[pos_] == QLatin1Char('_')))
				++pos_;
			return std::make_shared<Counter>(s_.mid(start, pos_ - start));
		}
		fail(pos_ < s_.size() ? QStringLiteral("unexpected '%1'").arg(s_.mid(pos_, 1))
				      : QStringLiteral("unexpected end"));
		return nullptr;
	}

	const QString &s_;
	qsizetype pos_ = 0;
	QString error_;
};

} // namespace

std::shared_ptr<const Expression> Expression::parse(const QString &text, QString *error)
{
	return Parser(text).parse(error);
}

bool Layout::fromJson(const QByteArray &json, Layout &out, QString *error)
{
	auto failWith = [error](const QString &message) {
		if (error)
			*error = message;
		return false;
	};

	QJsonParseError parseError;
	const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
	if (!doc.isObject())
		return failWith(parseError.errorString());
	if (doc.object().value(QStringLiteral("version")).toInt() != 1)
		return failWith(QStringLiteral("version must be 1"));

	Layout layout;
	QSet<QString> ids;
	for (const QJsonValue v : doc.object().value(QStringLiteral("columns")).toArray()) {
		const QJsonObject obj = v.toObject();
		Column col;
		col.id = obj.value(QStringLiteral("id")).toString();
		if (col.id.isEmpty() || ids.contains(col.id))
			return failWith(QStringLiteral("column without a unique id"));
		ids.insert(col.id);

		const QString kind = obj.value(QStringLiteral("kind")).toString(QStringLiteral("value"));
		if (kind == QLatin1String("class_icon"))
			col.kind = Column::Kind::ClassIcon;
		else if (kind == QLatin1String("tier"))
			col.kind = Column::Kind::Tier;
		else if (kind == QLatin1String("name"))
			col.kind = Column::Kind::Name;
		else if (kind == QLatin1String("value"))
			col.kind = Column::Kind::Value;
		else
			return failWith(QStringLiteral("column %1: unknown kind %2").arg(col.id, kind));

		col.header = obj.value(QStringLiteral("header")).toString();
		col.label = obj.value(QStringLiteral("label")).toString(col.header);
		col.decimals = obj.value(QStringLiteral("decimals")).toInt(0);
		col.suffix = obj.value(QStringLiteral("suffix")).toString();
		col.visibleByDefault = obj.value(QStringLiteral("visible")).toBool(true);
		for (const QJsonValue r : obj.value(QStringLiteral("ranges")).toArray()) {
			const QJsonObject band = r.toObject();
			const QJsonValue from = band.value(QStringLiteral("from"));
			col.ranges.append({from.isDouble() ? from.toDouble() : -std::numeric_limits<double>::infinity(),
					   band.value(QStringLiteral("color")).toString().toLower()});
		}
		col.ranges = parseRanges(formatRanges(col.ranges)); // sorted, first unbounded

		if (col.kind == Column::Kind::Value) {
			QString exprError;
			col.expr = Expression::parse(obj.value(QStringLiteral("expr")).toString(), &exprError);
			if (!col.expr)
				return failWith(QStringLiteral("column %1: %2").arg(col.id, exprError));
		}
		layout.columns_.append(col);
	}
	if (layout.columns_.isEmpty())
		return failWith(QStringLiteral("no columns"));

	out = layout;
	return true;
}

QList<ValueRange> parseRanges(const QString &text)
{
	QList<ValueRange> out;
	for (const QString &part : text.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
		const qsizetype colon = part.indexOf(QLatin1Char(':'));
		if (colon < 0)
			continue;
		const QString from = part.left(colon).trimmed();
		const QString color = part.mid(colon + 1).trimmed().toLower();
		bool ok = false;
		const double value = from.toDouble(&ok);
		if (!color.startsWith(QLatin1Char('#')))
			continue;
		out.append({ok ? value : -std::numeric_limits<double>::infinity(), color});
	}
	std::stable_sort(out.begin(), out.end(),
			 [](const ValueRange &a, const ValueRange &b) { return a.from < b.from; });
	if (!out.isEmpty())
		out.first().from = -std::numeric_limits<double>::infinity();
	return out;
}

QString formatRanges(const QList<ValueRange> &ranges)
{
	QStringList parts;
	for (const ValueRange &r : ranges)
		parts.append((std::isinf(r.from) ? QStringLiteral("-") : QString::number(r.from)) + QLatin1Char(':') +
			     r.color);
	return parts.join(QLatin1Char(';'));
}

QString rangeColor(const QList<ValueRange> &ranges, double value)
{
	QString color;
	for (const ValueRange &r : ranges)
		if (value >= r.from)
			color = r.color;
	return color;
}

QString romanTier(int tier)
{
	static const char *const numerals[] = {"I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X"};
	if (tier >= 1 && tier <= 10)
		return QString::fromLatin1(numerals[tier - 1]);
	return tier > 0 ? QString::number(tier) : QString();
}

} // namespace bss
