/*
 * Copyright (c) 2015 Nathan Osman
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include <QStringBuilder>

#include <QHttpEngine/QHttpHandler>
#include <QHttpEngine/QHttpMiddleware>
#include <QHttpEngine/QHttpSocket>

#include "qhttphandler_p.h"
#include <algorithm>


QHttpHandlerPrivate::QHttpHandlerPrivate(QHttpHandler *handler)
    : QObject(handler),
      q(handler)
{
}

QHttpHandler::QHttpHandler(QObject *parent)
    : QObject(parent),
      d(new QHttpHandlerPrivate(this))
{
}

void QHttpHandler::addMiddleware(QHttpMiddleware *middleware)
{
    d->middleware.append(middleware);
}

void QHttpHandler::addRedirect(const QRegExp &pattern, const QString &path)
{
    d->redirects.append(Redirect(pattern, path));
}

void QHttpHandler::removeRedirect(const QRegExp &pattern)
{
    d->redirects.erase(std::remove_if(d->redirects.begin(), d->redirects.end(), [pattern] (const Redirect & r) -> bool {
        return r.first == pattern;
    }));
}

void QHttpHandler::addAlias(const QRegExp &pattern, const QString &path)
{
    d->aliases.append(Aliases(pattern, path));
}

void QHttpHandler::addSubHandler(const QRegExp &pattern, QHttpHandler *handler)
{
    d->subHandlers.append(SubHandler(pattern, handler));
}

void QHttpHandler::removeSubHandler(const QRegExp &pattern)
{
    d->subHandlers.erase(std::remove_if(d->subHandlers.begin(), d->subHandlers.end(), [pattern] (const SubHandler & r) -> bool {
        return r.first == pattern;
    }));
}

void QHttpHandler::route(QHttpSocket *socket, const QString &path)
{
    // Run through each of the middleware
    foreach (QHttpMiddleware *middleware, d->middleware) {
        if (!middleware->process(socket)) {
            return;
        }
    }

    // Check each of the redirects for a match
    foreach(Redirect redirect, d->redirects) {
        if(redirect.first.indexIn(path) != -1) {
            QString newPath = redirect.second;
            foreach(QString replacement, redirect.first.capturedTexts().mid(1)) {
                newPath = newPath.arg(replacement);
            }
            socket->writeRedirect(newPath.toUtf8());
            return;
        }
    }

    // Check each of the sub-handlers for a match
    foreach(SubHandler subHandler, d->subHandlers) {
        if(subHandler.first.indexIn(path) != -1) {
            subHandler.second->route(socket, path.mid(subHandler.first.matchedLength()));
            return;
        }
    }

    // Check each of the aliases for a match
    foreach(Aliases alias, d->aliases) {
        if(alias.first.indexIn(path) != -1) {
            QString newPath = alias.second;
            foreach(QString replacement, alias.first.capturedTexts().mid(1)) {
                newPath = newPath.arg(replacement);
            }
            process(socket, newPath);
            return;
        }
    }

    // If no match, invoke the process() method
    process(socket, path);
}

void QHttpHandler::process(QHttpSocket *socket, const QString &)
{
    // The default response is simply a 404 error
    socket->writeError(QHttpSocket::NotFound);
    socket->close();
}

