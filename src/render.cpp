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
	if (table.columns == 0 ||
	    (table.header.isEmpty() && table.rows.isEmpty() && table.total.isEmpty() && table.caption.isEmpty()))
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
	QFont captionFont = style.font;
	captionFont.setBold(true);
	const QFontMetrics caption(captionFont);
	const int captionH = table.caption.isEmpty() ? 0 : caption.height();
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
	if (inside) {
		QList<Cell> labels = table.header;
		for (int i = 0; i < labels.size(); ++i)
			if (table.identity.value(i, false))
				labels[i].text.clear();
		measure(labels, label);
	} else {
		measure(table.header, header);
	}
	for (const auto &row : table.rows)
		measure(row, body);
	measure(table.total, total);
	// Number columns are as wide as the widest value they can expect, not the
	// one they hold now, so the table does not twitch as 999 becomes 1000.
	QList<Cell> samples;
	for (const QString &s : table.samples)
		samples.append(Cell{Cell::Kind::Text, Cell::Align::Right, s});
	measure(samples, total);

	int contentW = style.columnSpacing * std::max(0, table.columns - 1);
	for (int w : widths)
		contentW += w;
	// A nickname longer than the table widens the name column.
	if (captionH) {
		const int short_ = caption.horizontalAdvance(table.caption) - contentW;
		if (short_ > 0) {
			const qsizetype at = std::max<qsizetype>(0, table.stretch.indexOf(true));
			widths[at] += short_;
			contentW += short_;
		}
	}

	// A width the streamer set is a minimum: the name column takes what is
	// left over (every column a share when there is no name). A name wider
	// than that makes the table wider; nothing is ever cut.
	const int spare = style.minWidth - 2 * padX - contentW;
	if (spare > 0 && table.columns > 0) {
		const qsizetype stretchAt = table.stretch.indexOf(true);
		if (stretchAt >= 0) {
			widths[stretchAt] += spare;
		} else {
			for (int i = 0; i < table.columns; ++i)
				widths[i] += spare / table.columns;
		}
		contentW = style.minWidth - 2 * padX;
	}

	// Every data row is a block of the same height and the full width; the
	// header sits above them, aligned with the blocks' contents.
	const int blockH = lineH + labelH + 2 * padY;
	const int blocks = static_cast<int>(table.rows.size()) + (table.total.isEmpty() ? 0 : 1);
	const int width = std::max(contentW + 2 * padX, style.minWidth);
	int height = blocks * blockH + std::max(0, blocks - 1) * gap;
	if (headerH)
		height += headerH + (blocks ? gap : 0);
	if (captionH)
		height += captionH + (headerH || blocks ? gap : 0);
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
			QColor ink = color ? *color : style.columnColors.value(id, style.text);
			if (c.value) {
				const QString banded = rangeColor(style.ranges.value(id), *c.value);
				if (!banded.isEmpty())
					ink = QColor(banded);
			}
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
				p.drawText(box, static_cast<int>(align | Qt::AlignVCenter), c.text);
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
	if (captionH) {
		p.setFont(captionFont);
		p.setPen(style.caption);
		p.drawText(QRect(padX, y, width - 2 * padX, captionH),
			   static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter), table.caption);
		y += captionH + gap;
	}
	if (headerH) {
		drawCells(table.header, y, headerH, headerFont, &style.header);
		y += headerH + gap;
	}
	for (const auto &row : table.rows) {
		drawBlock(y);
		drawCells(row, y + padY, lineH, bodyFont, nullptr);
		if (inside)
			drawLabels(y + padY + lineH);
		y += blockH + gap;
	}
	if (!table.total.isEmpty()) {
		drawBlock(y);
		drawCells(table.total, y + padY, lineH, totalFont, &style.total);
		if (inside)
			drawLabels(y + padY + lineH);
	}
	p.end();
	return image;
}

} // namespace bss
