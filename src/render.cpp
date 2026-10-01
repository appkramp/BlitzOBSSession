#include "render.h"

#include <QDir>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QSvgRenderer>

#include <algorithm>
#include <cmath>

namespace bss {

Renderer::Renderer(QString iconDir) : iconDir_(std::move(iconDir)) {}

Renderer::~Renderer() = default;

QSvgRenderer *Renderer::icon(const QString &type)
{
	if (type.isEmpty())
		return nullptr;
	auto it = icons_.find(type);
	if (it == icons_.end()) {
		auto svg = std::make_shared<QSvgRenderer>(
			QDir(iconDir_).filePath(QStringLiteral("class-%1.svg").arg(type)));
		it = icons_.insert(type, svg->isValid() ? svg : nullptr);
	}
	return it.value().get();
}

QImage Renderer::render(const Table &table, const Style &style)
{
	if (table.columns == 0 || (table.header.isEmpty() && table.rows.isEmpty() && table.total.isEmpty()))
		return QImage();

	QFont bodyFont = style.font;
	QFont headerFont = style.font;
	headerFont.setPixelSize(std::max(6, static_cast<int>(std::lround(style.font.pixelSize() * 0.8))));
	QFont totalFont = style.font;
	totalFont.setBold(true);
	QFont labelFont = style.font;
	labelFont.setPixelSize(std::max(6, static_cast<int>(std::lround(style.font.pixelSize() * 0.42))));
	labelFont.setBold(false);
	labelFont.setItalic(false);

	const bool inside = style.labelsInside && !table.header.isEmpty();
	const QFontMetrics body(bodyFont), header(headerFont), total(totalFont), label(labelFont);
	const int lineH = std::max(body.height(), total.height());
	const int headerH = table.header.isEmpty() || inside ? 0 : header.height();
	const int labelH = inside ? label.height() : 0;
	const int iconSide = static_cast<int>(std::lround(lineH * 0.8));
	const int border = std::max(0, style.borderWidth);
	const int padX = std::max(0, style.paddingX) + border;
	const int padY = std::max(0, style.paddingY) + border;
	const int gap = std::max(0, style.rowSpacing);

	// Column widths: the widest cell of each column, in the font it is drawn in.
	QList<int> widths(table.columns, 0);
	auto measure = [&](const QList<Cell> &row, const QFontMetrics &fm) {
		for (int i = 0; i < row.size() && i < table.columns; ++i) {
			const Cell &c = row[i];
			const int w = c.kind == Cell::Kind::ClassIcon ? iconSide : fm.horizontalAdvance(c.text);
			widths[i] = std::max(widths[i], w);
		}
	};
	const bool fixed = style.fixedWidth > 0;
	if (inside) {
		QList<Cell> labels = table.header;
		for (int i = 0; i < labels.size(); ++i)
			if (table.identity.value(i, false))
				labels[i].text.clear();
		measure(labels, label);
	} else {
		measure(table.header, header);
	}
	if (fixed) {
		// Sized for what a column can hold, not for what it holds now.
		QList<Cell> samples;
		for (const QString &s : table.samples)
			samples.append(Cell{Cell::Kind::Text, Cell::Align::Right, s});
		measure(samples, total);
		for (int i = 0; i < table.columns; ++i)
			if (!table.stretch.value(i, false) && table.identity.value(i, false))
				widths[i] = std::max(widths[i], iconSide);
		// "Total" sits in a text column when the name is hidden.
		QList<Cell> totalLabels = table.total;
		for (int i = 0; i < totalLabels.size(); ++i)
			if (!table.identity.value(i, false) || table.stretch.value(i, false))
				totalLabels[i].text.clear();
		measure(totalLabels, total);
	} else {
		for (const auto &row : table.rows)
			measure(row, body);
		measure(table.total, total);
	}

	int contentW = style.columnSpacing * std::max(0, table.columns - 1);
	for (int i = 0; i < table.columns; ++i)
		contentW += table.stretch.value(i, false) && fixed ? 0 : widths[i];

	if (fixed) {
		// The name takes what is left; without one, every column gets a share.
		const int free = style.fixedWidth - 2 * padX - contentW;
		const qsizetype stretchAt = table.stretch.indexOf(true);
		if (stretchAt >= 0) {
			const int minimum = body.horizontalAdvance(QStringLiteral("WWWW"));
			widths[stretchAt] = std::max(free, minimum);
			contentW += widths[stretchAt];
		} else if (free > 0) {
			for (int i = 0; i < table.columns; ++i)
				widths[i] += free / table.columns;
			contentW += free / table.columns * table.columns;
		}
	}

	// Every data row is a block of the same height and the full width; the
	// header sits above them, aligned with the blocks' contents.
	const int blockH = lineH + labelH + 2 * padY;
	const int rowSlots = style.fixedRows >= 0 ? style.fixedRows : static_cast<int>(table.rows.size());
	const int blocks = rowSlots + (table.total.isEmpty() ? 0 : 1);
	const int width = std::max(contentW + 2 * padX, fixed ? style.fixedWidth : 0);
	int height = blocks * blockH + std::max(0, blocks - 1) * gap;
	if (headerH)
		height += headerH + (blocks ? gap : 0);
	if (width <= 0 || height <= 0)
		return QImage();

	QImage image(width, height, QImage::Format_RGBA8888_Premultiplied);
	image.fill(Qt::transparent);
	QPainter p(&image);
	p.setRenderHint(QPainter::Antialiasing);
	p.setRenderHint(QPainter::TextAntialiasing);
	p.setRenderHint(QPainter::SmoothPixmapTransform);
	p.setOpacity(std::clamp(style.opacity, 0.0, 1.0));

	auto drawBlock = [&](int y) {
		// Inset by half the border so the stroke stays inside the image.
		const double inset = border / 2.0;
		const QRectF rect(inset, y + inset, width - border, blockH - border);
		QPainterPath path;
		path.addRoundedRect(rect, style.radius, style.radius);
		p.fillPath(path, style.block);
		if (border > 0 && style.border.alpha() > 0) {
			QPen pen(style.border);
			pen.setWidth(border);
			p.strokePath(path, pen);
		}
	};

	// Class, tier and name carry no label inside the blocks; they sit in the
	// middle of the block's height instead of on the values' line.
	auto isIdentity = [&](int i) {
		return inside && table.identity.value(i, false);
	};

	// `color` overrides the per-column colours (header, total row).
	auto drawCells = [&](const QList<Cell> &row, int y, int h, const QFont &font, const QColor *color) {
		p.setFont(font);
		int x = padX;
		for (int i = 0; i < row.size() && i < table.columns; ++i) {
			const Cell &c = row[i];
			const QString id = table.columnIds.value(i);
			const QColor ink = color ? *color : style.columnColors.value(id, style.text);
			const QRect box = isIdentity(i) ? QRect(x, y, widths[i], h + labelH)
							: QRect(x, y, widths[i], h);
			if (c.kind == Cell::Kind::ClassIcon) {
				if (QSvgRenderer *svg = icon(c.text)) {
					// Keep the icon's own proportions inside the square.
					const QSizeF natural = svg->defaultSize();
					const double k =
						std::min(iconSide / natural.width(), iconSide / natural.height());
					const QSize size(
						std::max(1, static_cast<int>(std::lround(natural.width() * k))),
						std::max(1, static_cast<int>(std::lround(natural.height() * k))));
					QImage glyph(size, QImage::Format_ARGB32_Premultiplied);
					glyph.fill(Qt::transparent);
					{
						QPainter gp(&glyph);
						svg->render(&gp);
						gp.setCompositionMode(QPainter::CompositionMode_SourceIn);
						gp.fillRect(glyph.rect(), ink);
					}
					p.drawImage(QPointF(box.x() + (box.width() - size.width()) / 2.0,
							    box.y() + (box.height() - size.height()) / 2.0),
						    glyph);
				}
			} else if (!c.text.isEmpty()) {
				// Inside the blocks everything is centred but the name, which
				// starts at the column's left edge either way.
				const Qt::Alignment align = c.align == Cell::Align::Left ? Qt::AlignLeft
							    : inside || c.align == Cell::Align::Center
								    ? Qt::AlignHCenter
								    : Qt::AlignRight;
				p.setPen(ink);
				// A fixed-size table shortens a name that does not fit; numbers
				// are never cut, the columns are sized for them.
				const QString text =
					fixed && table.stretch.value(i, false)
						? QFontMetrics(font).elidedText(c.text, Qt::ElideRight, box.width())
						: c.text;
				p.drawText(box, static_cast<int>(align | Qt::AlignVCenter), text);
			}
			x += widths[i] + style.columnSpacing;
		}
	};

	// The column names under the values, small and centred.
	auto drawLabels = [&](int y) {
		p.setFont(labelFont);
		p.setPen(style.header);
		int x = padX;
		for (int i = 0; i < table.header.size() && i < table.columns; ++i) {
			const QString &text = table.header[i].text;
			if (!text.isEmpty() && !isIdentity(i))
				p.drawText(QRect(x, y, widths[i], labelH),
					   static_cast<int>(Qt::AlignHCenter | Qt::AlignTop), text);
			x += widths[i] + style.columnSpacing;
		}
	};

	int y = 0;
	if (headerH) {
		drawCells(table.header, y, headerH, headerFont, &style.header);
		y += headerH + gap;
	}
	for (qsizetype r = 0; r < table.rows.size() && r < rowSlots; ++r) {
		const auto &row = table.rows[r];
		drawBlock(y);
		drawCells(row, y + padY, lineH, bodyFont, nullptr);
		if (inside)
			drawLabels(y + padY + lineH);
		y += blockH + gap;
	}
	if (!table.total.isEmpty()) {
		// The total follows the rows; the room left for rows not yet played
		// stays empty below it, so the image keeps its size.
		drawBlock(y);
		drawCells(table.total, y + padY, lineH, totalFont, &style.total);
		if (inside)
			drawLabels(y + padY + lineH);
	}
	p.end();
	return image;
}

} // namespace bss
