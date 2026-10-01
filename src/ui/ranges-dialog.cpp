#include "ranges-dialog.h"

#include "plugin.h"

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

namespace bss {

namespace {

// A button that shows its colour and picks a new one.
void paintSwatch(QPushButton *button, const QString &color)
{
	button->setProperty("color", color);
	button->setText(color.toUpper());
	button->setStyleSheet(
		QStringLiteral("QPushButton { background: %1; color: %2; }")
			.arg(color, QColor(color).lightness() > 140 ? QStringLiteral("#000") : QStringLiteral("#fff")));
}

} // namespace

RangesDialog::RangesDialog(const QList<ColumnInfo> &columns, const QString &selected, QWidget *parent)
	: QDialog(parent),
	  columns_(columns)
{
	setWindowTitle(uiText("Ranges.Title"));
	setMinimumWidth(420);

	auto *hint = new QLabel(uiText("Ranges.Hint"));
	hint->setWordWrap(true);

	picker_ = new QComboBox;
	for (const ColumnInfo &c : columns_)
		picker_->addItem(c.label, c.id);

	auto *rowsBox = new QWidget;
	rows_ = new QVBoxLayout(rowsBox);
	rows_->setContentsMargins(0, 0, 0, 0);

	auto *add = new QPushButton(uiText("Ranges.Add"));
	connect(add, &QPushButton::clicked, this, [this] {
		harvest();
		QList<ValueRange> ranges = edited_.value(columns_[shown_].id);
		const double last = ranges.isEmpty() || std::isinf(ranges.last().from) ? 45.0
										       : ranges.last().from + 5.0;
		ranges.append({ranges.isEmpty() ? -std::numeric_limits<double>::infinity() : last,
			       QStringLiteral("#ffffff")});
		edited_.insert(columns_[shown_].id, ranges);
		reset_.remove(columns_[shown_].id);
		rebuild(ranges);
	});
	auto *defaults = new QPushButton(uiText("Ranges.Defaults"));
	connect(defaults, &QPushButton::clicked, this, [this] {
		const ColumnInfo &c = columns_[shown_];
		edited_.remove(c.id);
		reset_.insert(c.id);
		rebuild(c.defaults);
	});
	auto *actions = new QHBoxLayout;
	actions->addWidget(add);
	actions->addStretch(1);
	actions->addWidget(defaults);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, [this] {
		harvest();
		accept();
	});
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(hint);
	layout->addWidget(picker_);
	layout->addWidget(rowsBox);
	layout->addLayout(actions);
	layout->addStretch(1);
	layout->addWidget(buttons);

	connect(picker_, &QComboBox::currentIndexChanged, this, &RangesDialog::showColumn);
	const int start = std::max(0, picker_->findData(selected));
	picker_->setCurrentIndex(start);
	showColumn(start);
}

void RangesDialog::showColumn(int index)
{
	if (index < 0 || index >= columns_.size() || index == shown_)
		return;
	harvest();
	shown_ = index;
	const ColumnInfo &c = columns_[index];
	if (reset_.contains(c.id))
		rebuild(c.defaults);
	else
		rebuild(edited_.value(c.id, c.current));
}

// Reads the rows on screen back into edited_, sorted.
void RangesDialog::harvest()
{
	if (shown_ < 0)
		return;
	const ColumnInfo &c = columns_[shown_];
	QList<ValueRange> ranges;
	for (int i = 0; i < rows_->count(); ++i) {
		QWidget *row = rows_->itemAt(i)->widget();
		if (!row)
			continue;
		auto *spin = row->findChild<QDoubleSpinBox *>();
		auto *swatch = row->findChild<QPushButton *>();
		if (!spin || !swatch)
			continue;
		const double from = row->property("first").toBool() ? -std::numeric_limits<double>::infinity()
								    : spin->value();
		ranges.append({from, swatch->property("color").toString()});
	}
	ranges = parseRanges(formatRanges(ranges));
	const QList<ValueRange> original = reset_.contains(c.id) ? c.defaults : c.current;
	if (ranges != original || edited_.contains(c.id)) {
		edited_.insert(c.id, ranges);
		reset_.remove(c.id);
	}
}

void RangesDialog::rebuild(const QList<ValueRange> &ranges)
{
	while (QLayoutItem *item = rows_->takeAt(0)) {
		delete item->widget();
		delete item;
	}
	for (qsizetype i = 0; i < ranges.size(); ++i)
		addRow(ranges[i].from, ranges[i].color, i == 0);
	if (ranges.isEmpty()) {
		auto *none = new QLabel(uiText("Ranges.None"));
		none->setWordWrap(true);
		rows_->addWidget(none);
	}
}

void RangesDialog::addRow(double from, const QString &color, bool first)
{
	const ColumnInfo &c = columns_[shown_];
	auto *row = new QWidget;
	row->setProperty("first", first);
	auto *h = new QHBoxLayout(row);
	h->setContentsMargins(0, 0, 0, 0);

	auto *label = new QLabel(first ? uiText("Ranges.Below") : uiText("Ranges.From"));
	auto *spin = new QDoubleSpinBox;
	spin->setRange(-1e9, 1e9);
	spin->setDecimals(std::max(0, c.decimals));
	spin->setSuffix(c.suffix.isEmpty() ? QString() : QStringLiteral(" ") + c.suffix);
	spin->setValue(std::isinf(from) ? 0.0 : from);
	spin->setVisible(!first);

	auto *swatch = new QPushButton;
	swatch->setMinimumWidth(110);
	paintSwatch(swatch, color);
	connect(swatch, &QPushButton::clicked, this, [this, swatch] {
		const QColor picked = QColorDialog::getColor(QColor(swatch->property("color").toString()), this,
							     uiText("Ranges.Title"));
		if (picked.isValid())
			paintSwatch(swatch, picked.name());
	});

	auto *remove = new QToolButton;
	remove->setText(QStringLiteral("✕"));
	connect(remove, &QToolButton::clicked, this, [this, row] {
		rows_->removeWidget(row);
		row->deleteLater();
		row->hide();
		harvest();
		rebuild(edited_.value(columns_[shown_].id));
	});

	h->addWidget(label);
	h->addWidget(spin, 1);
	if (first)
		h->addStretch(1);
	h->addWidget(swatch);
	h->addWidget(remove);
	rows_->addWidget(row);
}

} // namespace bss
