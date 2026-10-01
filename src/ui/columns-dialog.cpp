#include "columns-dialog.h"

#include "plugin.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QVBoxLayout>

namespace bss {

ColumnsDialog::ColumnsDialog(const QList<Item> &items, QWidget *parent) : QDialog(parent)
{
	setWindowTitle(uiText("Columns.Title"));
	setMinimumSize(320, 380);

	auto *hint = new QLabel(uiText("Columns.Hint"));
	hint->setWordWrap(true);

	list_ = new QListWidget;
	list_->setDragDropMode(QAbstractItemView::InternalMove);
	list_->setDefaultDropAction(Qt::MoveAction);
	list_->setSelectionMode(QAbstractItemView::SingleSelection);
	for (const Item &it : items) {
		auto *row = new QListWidgetItem(it.label, list_);
		row->setData(Qt::UserRole, it.id);
		row->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable |
			      Qt::ItemIsDragEnabled);
		row->setCheckState(it.visible ? Qt::Checked : Qt::Unchecked);
	}
	list_->setCurrentRow(0);

	auto *up = new QPushButton(QStringLiteral("▲ ") + uiText("Columns.Up"));
	auto *down = new QPushButton(QStringLiteral("▼ ") + uiText("Columns.Down"));
	connect(up, &QPushButton::clicked, this, [this] { move(-1); });
	connect(down, &QPushButton::clicked, this, [this] { move(1); });
	auto *arrows = new QVBoxLayout;
	arrows->addWidget(up);
	arrows->addWidget(down);
	arrows->addStretch(1);

	auto *body = new QHBoxLayout;
	body->addWidget(list_, 1);
	body->addLayout(arrows);

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	auto *layout = new QVBoxLayout(this);
	layout->addWidget(hint);
	layout->addLayout(body, 1);
	layout->addWidget(buttons);
}

void ColumnsDialog::move(int delta)
{
	const int from = list_->currentRow();
	const int to = from + delta;
	if (from < 0 || to < 0 || to >= list_->count())
		return;
	QListWidgetItem *item = list_->takeItem(from);
	list_->insertItem(to, item);
	list_->setCurrentRow(to);
}

QList<ColumnsDialog::Item> ColumnsDialog::items() const
{
	QList<Item> out;
	for (int i = 0; i < list_->count(); ++i) {
		const QListWidgetItem *row = list_->item(i);
		out.append({row->data(Qt::UserRole).toString(), row->text(), row->checkState() == Qt::Checked});
	}
	return out;
}

} // namespace bss
