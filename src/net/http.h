#pragma once

// A single HTTPS GET on the operating system's own stack, for the vehicle
// catalogue. See websocket.h for why not Qt Network.

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

namespace bss::http {

// `status` is the HTTP status, or 0 with `error` set when there was no answer.
using Callback = std::function<void(int status, const QByteArray &body, const QString &error)>;

// Calls `done` on the Qt main thread, unless `context` was destroyed first.
void get(const QUrl &url, QObject *context, Callback done);

} // namespace bss::http
