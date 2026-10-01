// Windows: WinHTTP's WebSocket API and plain WinHTTP requests.
//
// WinHTTP is used synchronously on a worker thread per connection. Results are
// posted to the Qt main thread through qApp and checked there against a
// QPointer and a generation number, as on macOS (net-mac.mm).

#include "http.h"
#include "websocket.h"

#include <QCoreApplication>
#include <QPointer>

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace bss {

namespace {

void onMainThread(std::function<void()> fn)
{
	QMetaObject::invokeMethod(qApp, std::move(fn), Qt::QueuedConnection);
}

QString winError(const char *what, DWORD code)
{
	return QStringLiteral("%1 failed (WinHTTP %2)").arg(QString::fromLatin1(what)).arg(code);
}

struct Target {
	std::wstring host;
	INTERNET_PORT port = 0;
	std::wstring path;
	bool secure = false;
};

Target targetOf(const QUrl &url)
{
	Target t;
	const QString scheme = url.scheme().toLower();
	t.secure = scheme == QLatin1String("wss") || scheme == QLatin1String("https");
	t.host = url.host().toStdWString();
	t.port = static_cast<INTERNET_PORT>(url.port(t.secure ? 443 : 80));
	QString path = url.path(QUrl::FullyEncoded);
	if (path.isEmpty())
		path = QStringLiteral("/");
	if (url.hasQuery())
		path += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
	t.path = path.toStdWString();
	return t;
}

// Handles of one connection, shared by the worker thread and the main thread.
struct Connection {
	std::mutex mu;
	HINTERNET session = nullptr;
	HINTERNET connect = nullptr;
	HINTERNET socket = nullptr;
	bool closing = false;

	~Connection() { closeHandles(); }

	void closeHandles()
	{
		std::lock_guard<std::mutex> lock(mu);
		closing = true;
		// Closing the socket handle cancels a receive blocked on it.
		for (HINTERNET *h : {&socket, &connect, &session}) {
			if (*h) {
				WinHttpCloseHandle(*h);
				*h = nullptr;
			}
		}
	}
};

class WinWebSocket final : public WebSocket {
public:
	explicit WinWebSocket(QObject *parent) : WebSocket(parent) {}
	~WinWebSocket() override { close(); }

	void open(const QUrl &url) override
	{
		close();
		const quint64 gen = ++generation_;
		ended_ = false;
		conn_ = std::make_shared<Connection>();
		std::thread(&WinWebSocket::run, QPointer<WinWebSocket>(this), conn_, targetOf(url), gen).detach();
	}

	void sendText(const QByteArray &text) override
	{
		if (!conn_)
			return;
		DWORD err = ERROR_INVALID_HANDLE;
		{
			std::lock_guard<std::mutex> lock(conn_->mu);
			if (conn_->socket && !conn_->closing)
				err = WinHttpWebSocketSend(conn_->socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
							   const_cast<char *>(text.constData()),
							   static_cast<DWORD>(text.size()));
		}
		if (err != NO_ERROR)
			end(generation_, winError("send", err));
	}

	void close() override
	{
		++generation_;
		if (conn_) {
			conn_->closeHandles();
			conn_.reset();
		}
	}

private:
	static void run(QPointer<WinWebSocket> self, std::shared_ptr<Connection> conn, Target target, quint64 gen)
	{
		auto fail = [&](const QString &reason) {
			onMainThread([self, gen, reason] {
				if (self)
					self->end(gen, reason);
			});
		};

		HINTERNET request = nullptr;
		{
			std::lock_guard<std::mutex> lock(conn->mu);
			if (conn->closing)
				return;
			conn->session = WinHttpOpen(L"BlitzSessionStats", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
						    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (conn->session)
				conn->connect = WinHttpConnect(conn->session, target.host.c_str(), target.port, 0);
			if (conn->connect)
				request = WinHttpOpenRequest(conn->connect, L"GET", target.path.c_str(), nullptr,
							     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
							     target.secure ? WINHTTP_FLAG_SECURE : 0);
		}
		if (!request)
			return fail(winError("open", GetLastError()));

		DWORD status = 0;
		DWORD size = sizeof(status);
		const bool upgraded =
			WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
			WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
			WinHttpReceiveResponse(request, nullptr);
		if (!upgraded) {
			const DWORD err = GetLastError();
			WinHttpCloseHandle(request);
			return fail(winError("connect", err));
		}
		WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
				    WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
		if (status != 101) {
			WinHttpCloseHandle(request);
			return fail(QStringLiteral("HTTP %1").arg(status));
		}

		HINTERNET socket = WinHttpWebSocketCompleteUpgrade(request, 0);
		const DWORD upgradeError = GetLastError();
		WinHttpCloseHandle(request);
		if (!socket)
			return fail(winError("upgrade", upgradeError));
		{
			std::lock_guard<std::mutex> lock(conn->mu);
			if (conn->closing) {
				WinHttpCloseHandle(socket);
				return;
			}
			conn->socket = socket;
		}

		onMainThread([self, gen] {
			if (self && self->generation_ == gen)
				emit self->opened();
		});

		std::string buffer(64 * 1024, '\0');
		QByteArray message;
		for (;;) {
			DWORD read = 0;
			WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
			const DWORD err = WinHttpWebSocketReceive(socket, buffer.data(), static_cast<DWORD>(buffer.size()),
								  &read, &type);
			{
				std::lock_guard<std::mutex> lock(conn->mu);
				if (conn->closing)
					return;
			}
			if (err != NO_ERROR)
				return fail(winError("receive", err));

			switch (type) {
			case WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE:
			case WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE:
				message.append(buffer.data(), static_cast<qsizetype>(read));
				break;
			case WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE:
			case WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE: {
				message.append(buffer.data(), static_cast<qsizetype>(read));
				const QByteArray complete = message;
				message.clear();
				onMainThread([self, gen, complete] {
					if (self && self->generation_ == gen)
						emit self->textReceived(complete);
				});
				break;
			}
			case WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE:
			default:
				return fail(QStringLiteral("closed by the server"));
			}
		}
	}

	void end(quint64 gen, const QString &reason)
	{
		if (gen != generation_ || ended_)
			return;
		ended_ = true;
		close();
		emit closed(reason);
	}

	std::shared_ptr<Connection> conn_;
	quint64 generation_ = 0;
	bool ended_ = false;
};

} // namespace

WebSocket *WebSocket::create(QObject *parent)
{
	return new WinWebSocket(parent);
}

namespace http {

void get(const QUrl &url, QObject *context, Callback done)
{
	QPointer<QObject> guard(context);
	std::thread([guard, done, target = targetOf(url)] {
		int status = 0;
		QByteArray body;
		QString error;

		HINTERNET session = WinHttpOpen(L"BlitzSessionStats", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
						WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		HINTERNET connect = session ? WinHttpConnect(session, target.host.c_str(), target.port, 0) : nullptr;
		HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", target.path.c_str(), nullptr,
								 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
								 target.secure ? WINHTTP_FLAG_SECURE : 0)
					    : nullptr;
		if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0,
						  0, 0) &&
		    WinHttpReceiveResponse(request, nullptr)) {
			DWORD code = 0;
			DWORD size = sizeof(code);
			WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
					    WINHTTP_HEADER_NAME_BY_INDEX, &code, &size, WINHTTP_NO_HEADER_INDEX);
			status = static_cast<int>(code);
			for (;;) {
				DWORD available = 0;
				if (!WinHttpQueryDataAvailable(request, &available)) {
					error = winError("read", GetLastError());
					break;
				}
				if (available == 0)
					break;
				const qsizetype at = body.size();
				body.resize(at + static_cast<qsizetype>(available));
				DWORD read = 0;
				if (!WinHttpReadData(request, body.data() + at, available, &read)) {
					error = winError("read", GetLastError());
					break;
				}
				body.resize(at + static_cast<qsizetype>(read));
			}
		} else {
			error = winError("request", GetLastError());
		}
		for (HINTERNET h : {request, connect, session})
			if (h)
				WinHttpCloseHandle(h);
		if (!error.isEmpty())
			status = 0;

		onMainThread([guard, done, status, body, error] {
			if (guard)
				done(status, body, error);
		});
	}).detach();
}

} // namespace http

} // namespace bss
