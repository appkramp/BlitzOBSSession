#pragma once

// "Colour by value…": bands of colour for a number column — below 45 % red,
// from 45 % grey, and so on. Opened from a source's properties; edits every
// number column, one at a time.

#include "core/layout.h"

#include <QDialog>
#include <QMap>
#include <QSet>

class QComboBox;
class QVBoxLayout;

namespace bss {

class RangesDialog : public QDialog {
	Q_OBJECT

public:
	struct ColumnInfo {
		QString id;
		QString label;
		int decimals = 0;
		QString suffix;
		QList<ValueRange> defaults;
		QList<ValueRange> current;
	};

	RangesDialog(const QList<ColumnInfo> &columns, const QString &selected, QWidget *parent);

	// What changed: a column's new bands, or a reset to the template's.
	QMap<QString, QList<ValueRange>> edited() const { return edited_; }
	QSet<QString> resetToDefault() const { return reset_; }

private:
	void showColumn(int index);
	void harvest();
	void addRow(double from, const QString &color, bool first);
	void rebuild(const QList<ValueRange> &ranges);

	QList<ColumnInfo> columns_;
	int shown_ = -1;
	QComboBox *picker_;
	QVBoxLayout *rows_;
	QMap<QString, QList<ValueRange>> edited_;
	QSet<QString> reset_;
};

} // namespace bss
