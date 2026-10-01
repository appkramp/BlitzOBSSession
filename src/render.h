#pragma once

// Paints a Table (core/table.h) into an image for an OBS texture: the header
// as plain text, then every data row — the total too — in its own rounded,
// translucent block.

#include "core/table.h"

#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QString>

#include <memory>

class QSvgRenderer;

namespace bss {

struct Style {
	QFont font; // pixel size set; the header is drawn at 80%

	// Text of the data rows, per column id; a column without one uses `text`.
	QHash<QString, QColor> columnColors;
	QColor text = QColor(255, 255, 255);
	QColor header = QColor(180, 190, 200);
	QColor total = QColor(255, 204, 64); // every cell of the total row

	QColor block = QColor(12, 14, 20, 178); // fill of a row's block, alpha included
	QColor border = QColor(255, 255, 255, 56);
	int borderWidth = 1;
	int radius = 5;

	// Column names under the values, inside every block, instead of a header
	// line above the table. Everything is centred then.
	bool labelsInside = false;

	double opacity = 1.0; // of the whole overlay
	int paddingX = 12;    // inside a block, left and right
	int paddingY = 6;     // inside a block, top and bottom
	int columnSpacing = 18;
	int rowSpacing = 6; // between blocks
};

class Renderer {
public:
	// `iconDir` holds class-<type>.svg for each vehicle type.
	explicit Renderer(QString iconDir);
	~Renderer();

	// Premultiplied RGBA, the layout OBS's GS_RGBA textures take.
	QImage render(const Table &table, const Style &style);

private:
	QSvgRenderer *icon(const QString &type);

	QString iconDir_;
	QHash<QString, std::shared_ptr<QSvgRenderer>> icons_;
};

} // namespace bss
