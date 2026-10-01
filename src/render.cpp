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
		auto svg = std::make_shared<QSvgRenderer>(QDir(iconDir_).filePath(QStringLiteral("class-%1.svg").arg(type)));
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

	const QFontMetrics body(bodyFont), header(headerFont), total(totalFont);
	const int lineH = std::max(body.height(), total.height());
	const int headerH = table.header.isEmpty() ? 0 : header.height();
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
	measure(table.header, header);
	for (const auto &row : table.rows)
		measure(row, body);
	measure(table.total, total);

	int contentW = style.columnSpacing * std::max(0, table.columns - 1);
	for (int w : widths)
		contentW += w;

	// Every data row is a block of the same height and the full width; the
	// header sits above them, aligned with the blocks' contents.
	const int blockH = lineH + 2 * padY;
	const int blocks = static_cast<int>(table.rows.size()) + (table.total.isEmpty() ? 0 : 1);
	const int width = contentW + 2 * padX;
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

	// `color` overrides the per-column colours (header, total row).
	auto drawCells = [&](const QList<Cell> &row, int y, int h, const QFont &font, const QColor *color) {
		p.setFont(font);
		int x = padX;
		for (int i = 0; i < row.size() && i < table.columns; ++i) {
			const Cell &c = row[i];
			const QString id = table.columnIds.value(i);
			const QColor ink = color ? *color : style.columnColors.value(id, style.text);
			const QRect box(x, y, widths[i], h);
			if (c.kind == Cell::Kind::ClassIcon) {
				if (QSvgRenderer *svg = icon(c.text)) {
					// Keep the icon's own proportions inside the square.
					const QSizeF natural = svg->defaultSize();
					const double k = std::min(iconSide / natural.width(), iconSide / natural.height());
					const QSize size(std::max(1, static_cast<int>(std::lround(natural.width() * k))),
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
				const Qt::Alignment align = c.align == Cell::Align::Left     ? Qt::AlignLeft
							    : c.align == Cell::Align::Center ? Qt::AlignHCenter
											     : Qt::AlignRight;
				p.setPen(ink);
				p.drawText(box, static_cast<int>(align | Qt::AlignVCenter), c.text);
			}
			x += widths[i] + style.columnSpacing;
		}
	};

	int y = 0;
	if (headerH) {
		drawCells(table.header, y, headerH, headerFont, &style.header);
		y += headerH + gap;
	}
	for (const auto &row : table.rows) {
		drawBlock(y);
		drawCells(row, y + padY, lineH, bodyFont, nullptr);
		y += blockH + gap;
	}
	if (!table.total.isEmpty()) {
		drawBlock(y);
		drawCells(table.total, y + padY, lineH, totalFont, &style.total);
	}
	p.end();
	return image;
}

} // namespace bss
