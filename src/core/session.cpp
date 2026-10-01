#include "session.h"

#include <QJsonValue>

#include <algorithm>

namespace bss {

void Counters::add(const QJsonObject &all)
{
	for (auto it = all.begin(); it != all.end(); ++it) {
		if (it.value().isDouble())
			values_[it.key()] += it.value().toDouble();
	}
}

void Counters::add(const Counters &other)
{
	for (auto it = other.values_.begin(); it != other.values_.end(); ++it)
		values_[it.key()] += it.value();
}

bool BattleRecord::fromJson(const QJsonObject &obj, BattleRecord &out)
{
	const QJsonValue id = obj.value(QStringLiteral("id"));
	const QJsonValue tank = obj.value(QStringLiteral("tank_id"));
	if (!id.isDouble() || !tank.isDouble())
		return false;

	out.id = static_cast<qint64>(id.toDouble());
	out.tankId = tank.toInt();
	out.date = QDateTime::fromString(obj.value(QStringLiteral("date")).toString(), Qt::ISODate);
	out.count = obj.value(QStringLiteral("count")).toInt(1);
	out.approximate = obj.value(QStringLiteral("approximate")).toBool();
	out.all = obj.value(QStringLiteral("values")).toObject().value(QStringLiteral("all")).toObject();
	return out.id > 0 && out.tankId > 0;
}

bool Session::add(const BattleRecord &record)
{
	if (ids_.contains(record.id))
		return false;
	ids_.insert(record.id);
	lastId_ = std::max(lastId_, record.id);

	TankTotals &tank = tanks_[record.tankId];
	tank.tankId = record.tankId;
	tank.counters.add(record.all);
	totals_.add(record.all);

	if (record.date.isValid()) {
		if (!tank.lastBattle.isValid() || record.date > tank.lastBattle)
			tank.lastBattle = record.date;
		if (!lastBattle_.isValid() || record.date > lastBattle_)
			lastBattle_ = record.date;
	}
	return true;
}

void Session::clear()
{
	ids_.clear();
	lastId_ = 0;
	tanks_.clear();
	totals_ = Counters();
	lastBattle_ = QDateTime();
}

QList<TankTotals> Session::ordered(Order order) const
{
	QList<TankTotals> list = tanks_.values();
	std::stable_sort(list.begin(), list.end(), [order](const TankTotals &a, const TankTotals &b) {
		if (order == Order::Battles) {
			const double ba = a.counters.value(QStringLiteral("battles"));
			const double bb = b.counters.value(QStringLiteral("battles"));
			if (ba != bb)
				return ba > bb;
		}
		if (a.lastBattle != b.lastBattle)
			return a.lastBattle > b.lastBattle;
		return a.tankId < b.tankId;
	});
	return list;
}

} // namespace bss
