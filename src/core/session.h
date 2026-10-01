#pragma once

// The statistics of one period: every battle record the server sent, summed
// per vehicle and in total. Nothing here knows about OBS or the network.

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QSet>
#include <QString>

namespace bss {

// Numeric fields of `values.all`, summed. Kept generic — every numeric field
// the server sends is summed — so a column in layout.json can use any counter
// without a change here.
class Counters {
public:
	void add(const QJsonObject &all);
	void add(const Counters &other);
	double value(const QString &name) const { return values_.value(name, 0.0); }
	const QHash<QString, double> &values() const { return values_; }
	bool isEmpty() const { return values_.isEmpty(); }

private:
	QHash<QString, double> values_;
};

// One element of `battles` in a `history` or `battles` message.
struct BattleRecord {
	qint64 id = 0;
	int tankId = 0;
	QDateTime date;
	int count = 0;
	bool approximate = false;
	QJsonObject all; // values.all

	static bool fromJson(const QJsonObject &obj, BattleRecord &out);
};

struct TankTotals {
	int tankId = 0;
	Counters counters;
	QDateTime lastBattle;
};

class Session {
public:
	// Adds a record unless one with the same id is already held. Returns
	// whether it was added.
	bool add(const BattleRecord &record);
	void clear();

	// The largest record id held — the resume cursor (`after_id`).
	qint64 lastId() const { return lastId_; }
	bool isEmpty() const { return tanks_.isEmpty(); }

	const Counters &totals() const { return totals_; }
	const QMap<int, TankTotals> &tanks() const { return tanks_; }
	QDateTime lastBattle() const { return lastBattle_; }

	// Vehicles ordered for display: most recently played first, or most
	// battles first; ties broken by tank id so the order is stable.
	enum class Order { LastPlayed, Battles };
	QList<TankTotals> ordered(Order order) const;

private:
	QSet<qint64> ids_;
	qint64 lastId_ = 0;
	QMap<int, TankTotals> tanks_;
	Counters totals_;
	QDateTime lastBattle_;
};

} // namespace bss
