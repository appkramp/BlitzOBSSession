// macOS: NSURLSessionWebSocketTask and NSURLSession data tasks.
//
// NSURLSession calls back on its own queue. Everything is posted to the Qt
// main thread through qApp, and checked there against a QPointer and a
// generation number, so a callback from a connection that has since been
// closed or replaced is dropped instead of reaching the new one.

#include "http.h"
#include "websocket.h"

#include <QCoreApplication>
#include <QPointer>

#import <Foundation/Foundation.h>

namespace {

void onMainThread(std::function<void()> fn)
{
	QMetaObject::invokeMethod(qApp, std::move(fn), Qt::QueuedConnection);
}

QString describe(NSError *error)
{
	return error ? QString::fromNSString(error.localizedDescription) : QStringLiteral("connection closed");
}

} // namespace

@interface BSSWebSocketDelegate : NSObject <NSURLSessionWebSocketDelegate>
@property (nonatomic, copy) void (^onOpen)(void);
@property (nonatomic, copy) void (^onEnd)(NSString *reason);
@end

@implementation BSSWebSocketDelegate

- (void)URLSession:(NSURLSession *)session
		webSocketTask:(NSURLSessionWebSocketTask *)task
	didOpenWithProtocol:(NSString *)protocol
{
	(void)session;
	(void)task;
	(void)protocol;
	if (self.onOpen)
		self.onOpen();
}

- (void)URLSession:(NSURLSession *)session
		webSocketTask:(NSURLSessionWebSocketTask *)task
	didCloseWithCode:(NSURLSessionWebSocketCloseCode)code
		   reason:(NSData *)reason
{
	(void)session;
	(void)task;
	NSString *text = reason.length ? [[NSString alloc] initWithData:reason encoding:NSUTF8StringEncoding] : nil;
	if (self.onEnd)
		self.onEnd([NSString stringWithFormat:@"closed by the server (%ld%@%@)", (long)code, text ? @": " : @"",
						      text ?: @""]);
}

- (void)URLSession:(NSURLSession *)session task:(NSURLSessionTask *)task didCompleteWithError:(NSError *)error
{
	(void)session;
	NSHTTPURLResponse *response = [task.response isKindOfClass:[NSHTTPURLResponse class]]
					      ? (NSHTTPURLResponse *)task.response
					      : nil;
	NSString *reason = error ? error.localizedDescription : @"connection closed";
	if (response && response.statusCode != 101)
		reason = [NSString stringWithFormat:@"HTTP %ld", (long)response.statusCode];
	if (self.onEnd)
		self.onEnd(reason);
}

@end

namespace bss {

namespace {

class MacWebSocket final : public WebSocket {
public:
	explicit MacWebSocket(QObject *parent) : WebSocket(parent) {}
	~MacWebSocket() override { close(); }

	void open(const QUrl &url) override
	{
		close();
		const quint64 gen = ++generation_;
		ended_ = false;
		QPointer<MacWebSocket> self(this);

		BSSWebSocketDelegate *delegate = [[BSSWebSocketDelegate alloc] init];
		delegate.onOpen = ^{
			onMainThread([self, gen] {
				if (self && self->generation_ == gen)
					emit self->opened();
			});
		};
		delegate.onEnd = ^(NSString *reason) {
			const QString r = QString::fromNSString(reason);
			onMainThread([self, gen, r] {
				if (self)
					self->end(gen, r);
			});
		};

		NSURLSessionConfiguration *config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		config.timeoutIntervalForRequest = 30;
		session_ = [NSURLSession sessionWithConfiguration:config delegate:delegate delegateQueue:nil];
		task_ = [session_ webSocketTaskWithURL:url.toNSURL()];
		// History pages are larger than the default 1 MB limit allows for.
		task_.maximumMessageSize = 16 * 1024 * 1024;
		[task_ resume];
		receive(gen);
	}

	void sendText(const QByteArray &text) override
	{
		if (!task_)
			return;
		const quint64 gen = generation_;
		QPointer<MacWebSocket> self(this);
		NSString *string = [[NSString alloc] initWithBytes:text.constData()
							    length:static_cast<NSUInteger>(text.size())
							  encoding:NSUTF8StringEncoding];
		NSURLSessionWebSocketMessage *message = [[NSURLSessionWebSocketMessage alloc] initWithString:string];
		[task_ sendMessage:message
			completionHandler:^(NSError *error) {
				if (!error)
					return;
				const QString r = describe(error);
				onMainThread([self, gen, r] {
					if (self)
						self->end(gen, r);
				});
			}];
	}

	void close() override
	{
		++generation_;
		if (task_)
			[task_ cancelWithCloseCode:NSURLSessionWebSocketCloseCodeNormalClosure reason:nil];
		// The session holds its delegate strongly until invalidated.
		if (session_)
			[session_ invalidateAndCancel];
		task_ = nil;
		session_ = nil;
	}

private:
	void receive(quint64 gen)
	{
		QPointer<MacWebSocket> self(this);
		[task_ receiveMessageWithCompletionHandler:^(NSURLSessionWebSocketMessage *message, NSError *error) {
			if (error) {
				const QString r = describe(error);
				onMainThread([self, gen, r] {
					if (self)
						self->end(gen, r);
				});
				return;
			}
			QByteArray bytes;
			if (message.type == NSURLSessionWebSocketMessageTypeString) {
				NSData *data = [message.string dataUsingEncoding:NSUTF8StringEncoding];
				bytes = QByteArray(static_cast<const char *>(data.bytes), static_cast<qsizetype>(data.length));
			} else {
				bytes = QByteArray(static_cast<const char *>(message.data.bytes),
						   static_cast<qsizetype>(message.data.length));
			}
			onMainThread([self, gen, bytes] {
				if (!self || self->generation_ != gen)
					return;
				emit self->textReceived(bytes);
				// Ask for the next one only now, so messages keep their order
				// and a closed connection stops asking.
				if (self->generation_ == gen && self->task_)
					self->receive(gen);
			});
		}];
	}

	void end(quint64 gen, const QString &reason)
	{
		if (gen != generation_ || ended_)
			return;
		ended_ = true;
		close();
		emit closed(reason);
	}

	NSURLSession *session_ = nil;
	NSURLSessionWebSocketTask *task_ = nil;
	quint64 generation_ = 0;
	bool ended_ = false;
};

} // namespace

WebSocket *WebSocket::create(QObject *parent)
{
	return new MacWebSocket(parent);
}

namespace http {

void get(const QUrl &url, QObject *context, Callback done)
{
	QPointer<QObject> guard(context);
	NSMutableURLRequest *request = [NSMutableURLRequest requestWithURL:url.toNSURL()];
	request.timeoutInterval = 60;
	request.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
	NSURLSessionDataTask *task = [[NSURLSession sharedSession]
		dataTaskWithRequest:request
		  completionHandler:^(NSData *data, NSURLResponse *response, NSError *error) {
			  const int status = [response isKindOfClass:[NSHTTPURLResponse class]]
						     ? static_cast<int>(((NSHTTPURLResponse *)response).statusCode)
						     : 0;
			  const QByteArray body = data ? QByteArray(static_cast<const char *>(data.bytes),
								    static_cast<qsizetype>(data.length))
						       : QByteArray();
			  const QString message = error ? describe(error) : QString();
			  onMainThread([guard, done, status, body, message] {
				  if (guard)
					  done(status, body, message);
			  });
		  }];
	[task resume];
}

} // namespace http

} // namespace bss
