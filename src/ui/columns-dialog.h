#pragma once

// A source's columns: which are shown and in what order. Opened from the
// source's properties; a list with checkboxes, reordered by dragging or with
// the arrow buttons.

#include <QDialog>
#include <QList>
#include <QString>

class QListWidget;

namespace bss {

class ColumnsDialog : public QDialog {
	Q_OBJECT

public:
	struct Item {
		QString id;
		QString label;
		bool visible = true;
	};

	ColumnsDialog(const QList<Item> &items, QWidget *parent);

	// In display order.
	QList<Item> items() const;

private:
	void move(int delta);

	QListWidget *list_;
};

} // namespace bss
