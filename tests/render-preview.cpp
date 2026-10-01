// Paints an overlay into a PNG, to look at the renderer without OBS.
//
//   render-preview <vehicles.json> <out.png> [ru|en|uk] [font px] [inside|top] [min width]

#include "core/table.h"
#include "render.h"

#include <QFile>
#include <QGuiApplication>
#include <QJsonObject>
#include <QMap>
#include <QPainter>

#include <cstdio>

using namespace bss;

int main(int argc, char **argv)
{
	qputenv("QT_QPA_PLATFORM", "offscreen");
	QGuiApplication app(argc, argv);
	if (argc < 3) {
		std::fprintf(stderr, "usage: render-preview <vehicles.json> <out.png> [lang] [px]\n");
		return 2;
	}
	QFile f(QString::fromLocal8Bit(argv[1]));
	Catalog catalog;
	if (!f.open(QIODevice::ReadOnly) || !Catalog::parse(f.readAll(), catalog)) {
		std::fprintf(stderr, "cannot read the catalogue\n");
		return 1;
	}
	QFile lf(QStringLiteral(BSS_SOURCE_DIR "/data/layout.json"));
	lf.open(QIODevice::ReadOnly);
	Layout layout;
	Layout::fromJson(lf.readAll(), layout, nullptr);

	const struct {
		int tank;
		int wins, damage, hits, shots, survived, frags;
	} battles[] = {
		{3649, 1, 2140, 8, 10, 1, 2}, {3649, 0, 1310, 5, 9, 0, 1}, {3649, 1, 2890, 9, 11, 1, 3},
		{817, 1, 3120, 6, 8, 1, 2},   {817, 1, 2410, 5, 7, 0, 1},  {3937, 0, 1980, 4, 6, 0, 0},
		{2609, 1, 1250, 7, 12, 1, 2},
	};
	Session session;
	qint64 id = 1;
	for (const auto &b : battles) {
		BattleRecord r;
		r.id = id;
		r.tankId = b.tank;
		r.date = QDateTime(QDate(2026, 9, 29), QTime(18, 0)).addSecs(id * 420);
		r.all = QJsonObject{{"battles", 1},        {"wins", b.wins},         {"damage_dealt", b.damage},
				    {"hits", b.hits},      {"shots", b.shots},       {"survived_battles", b.survived},
				    {"frags", b.frags}};
		session.add(r);
		++id;
	}

	const QString lang = argc > 3 ? QString::fromLatin1(argv[3]) : QStringLiteral("ru");
	const QMap<QString, QString> ru = {{"Overlay.Col.Tier", "Ур."},       {"Overlay.Col.Tank", "Танк"},
					   {"Overlay.Col.Battles", "Бои"},    {"Overlay.Col.WinRate", "Победы"},
					   {"Overlay.Col.Damage", "Ср. урон"}, {"Overlay.Col.Accuracy", "Точность"},
					   {"Overlay.Col.Survival", "Выжив."}, {"Overlay.Col.Frags", "Фраги/бой"},
					   {"Overlay.Total", "Итого"}};
	TableOptions o;
	o.language = lang;
	for (const Column &c : layout.columns())
		if (c.visibleByDefault)
			o.columns << c.id;
	const Table table = buildTable(layout, session, catalog, o,
				       [&](const QString &k) { return ru.value(k, k.section('.', -1)); });

	Style style;
	style.font = QFont(QStringLiteral("Helvetica Neue"));
	style.font.setPixelSize(argc > 4 ? atoi(argv[4]) : 28);
	style.columnColors = {{"name", QColor(255, 255, 255)},     {"damage", QColor(255, 170, 60)},
			      {"winrate", QColor(110, 220, 120)}, {"tier", QColor(200, 206, 216)},
			      {"accuracy", QColor(120, 190, 255)}};
	style.labelsInside = argc > 5 && QByteArray(argv[5]) == "inside";
	if (argc > 6)
		style.minWidth = atoi(argv[6]);
	Renderer renderer(QStringLiteral(BSS_SOURCE_DIR "/data/icons"));
	QImage image = renderer.render(table, style);

	// On a mid-grey checkerboard, the way OBS shows a transparent source.
	QImage canvas(image.width() + 80, image.height() + 80, QImage::Format_ARGB32_Premultiplied);
	QPainter p(&canvas);
	for (int y = 0; y < canvas.height(); y += 20)
		for (int x = 0; x < canvas.width(); x += 20)
			p.fillRect(x, y, 20, 20, ((x + y) / 20) % 2 ? QColor(90, 110, 90) : QColor(120, 140, 120));
	p.drawImage(40, 40, image);
	p.end();
	return canvas.save(QString::fromLocal8Bit(argv[2])) ? 0 : 1;
}
