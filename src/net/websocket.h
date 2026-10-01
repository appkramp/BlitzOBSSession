#pragma once

// A WebSocket client on the operating system's own stack: NSURLSession on
// macOS, WinHTTP on Windows. OBS ships no TLS for Qt Network on macOS, and the
// system stack brings the system's certificates and proxy settings with it.
//
// Lives on the Qt main thread; every signal is delivered there.

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

namespace bss {

class WebSocket : public QObject {
	Q_OBJECT

public:
	static WebSocket *create(QObject *parent);

	// Starts connecting; a connection already open is closed first, silently.
	virtual void open(const QUrl &url) = 0;
	virtual void sendText(const QByteArray &text) = 0;
	// Closes without emitting closed(): the caller knows why.
	virtual void close() = 0;

signals:
	void opened();
	void textReceived(const QByteArray &text);
	// The connection failed or ended on its own. Emitted at most once per open().
	void closed(const QString &reason);

protected:
	using QObject::QObject;
};

} // namespace bss
