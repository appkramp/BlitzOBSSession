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

// A picture beside the table: a streamer's logo, say. Each source has its own.
struct Logo {
	enum class Side { Left, Right, Top, Bottom };
	enum class Align { Start, Center, End }; // along that side: top/left first
	enum class Mode { Fit, Stretch, Original };

	bool show = false;
	QString path;
	Side side = Side::Left;
	Align align = Align::Center;
	Mode mode = Mode::Fit;
	int width = 96; // the box it is drawn in; not used by Original
	int height = 96;
	int gap = 12;         // between it and the table
	double opacity = 1.0; // its own; the table's is Style::opacity
};

struct Style {
	QFont font; // pixel size set; the header is drawn at 80%

	// Text of the data rows, per column id; a column without one uses `text`.
	QHash<QString, QColor> columnColors;
	QColor text = QColor(255, 255, 255);
	QColor header = QColor(180, 190, 200);
	QColor total = QColor(255, 204, 64);    // every cell of the total row
	QColor caption = QColor(255, 255, 255); // the nickname above the table
	QFont captionFont;                      // its font; without a pixel size, the body's in bold
	Logo logo;
	// Value-dependent colours per column id; they win over the colours above
	// wherever a number is shown, the total row included.
	QHash<QString, QList<ValueRange>> ranges;

	QColor block = QColor(12, 14, 20, 178); // fill of a row's block, alpha included
	QColor border = QColor(255, 255, 255, 56);
	int borderWidth = 1;
	int radius = 5;

	// Column names under the values, inside every block, instead of a header
	// line above the table. Everything is centred then.
	bool labelsInside = false;

	// The least width of the table; 0 for as wide as the content. The name
	// column takes what is left over; the table grows wider still when a name
	// does not fit. Its height always follows the rows.
	int minWidth = 0;

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

	// Premultiplied RGBA, the layout OBS's GS_RGBA textures take: the table
	// and the logo beside it. `tableWidth`, when given, gets the table's own
	// width — what the width setting governs.
	QImage render(const Table &table, const Style &style, int *tableWidth = nullptr);

private:
	QImage renderTable(const Table &table, const Style &style);
	QImage logo(const Logo &logo);
	QSvgRenderer *icon(const QString &type);

	QString iconDir_;
	QHash<QString, std::shared_ptr<QSvgRenderer>> icons_;
	// Logo files read, by path (and box, for an SVG), kept while the file's
	// modification time holds.
	struct CachedLogo {
		qint64 stamp = -1;
		QImage image;
	};
	QHash<QString, CachedLogo> logos_;
};

} // namespace bss
