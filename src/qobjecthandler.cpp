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

#include <iostream>
#include <utility>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMetaMethod>
#include <QMetaObject>
#include <QMetaType>
#include <QStringList>
#include <QVariantMap>
#include <QStringBuilder>

#include "QHttpEngine/qobjecthandler.h"
#include "qobjecthandler_p.h"

QObjectHandlerPrivate::QObjectHandlerPrivate(QObjectHandler *handler)
    : QObject(handler),
      q(handler)
{
}

void QObjectHandlerPrivate::invokeSlot(QHttpSocket *socket, int index)
{
    q->queryString = socket->queryString();
    q->httpStatusCode = -1;

    QVariantMap retVal;
    QByteArray rawBody = socket->readAll();

    // Attempt to invoke the slot
    QMetaMethod wsMethod = q->metaObject()->method(index);

    if(socket->method() == QHttpSocket::HTTP_GET || socket->method() == QHttpSocket::HTTP_DELETE) {
        if(!wsMethod.invoke(q, Q_RETURN_ARG(QVariantMap, retVal))) {
            socket->writeError(QHttpSocket::InternalServerError);
            return;
        }
    } else {
        if(wsMethod.parameterType(0) == QMetaType::QByteArray) {
            if(!wsMethod.invoke(q,
                    Q_RETURN_ARG(QVariantMap, retVal),
                    Q_ARG(QByteArray, rawBody))) {
                socket->writeError(QHttpSocket::InternalServerError);
                return;
            }
        } else {
            // Attempt to decode the JSON from the socket
            QJsonParseError error;
            QJsonDocument document = QJsonDocument::fromJson(rawBody, &error);

            // Ensure that the document is valid
            if(error.error != QJsonParseError::NoError) {

                socket->writeError(QHttpSocket::BadRequest);
                return;
            }

            if(!wsMethod.invoke(q,
                    Q_RETURN_ARG(QVariantMap, retVal),
                    Q_ARG(QVariantMap, document.object().toVariantMap()))) {
                socket->writeError(QHttpSocket::InternalServerError);
                return;
            }
        }
    }

    // Convert the return value to JSON and write it to the socket
    if(q->httpStatusCode > -1)
        socket->setStatusCode(q->httpStatusCode);
    socket->writeHeaders();
    if(!retVal.isEmpty()) {
        QByteArray data = QJsonDocument(QJsonObject::fromVariantMap(retVal)).toJson();
        socket->setHeader("Content-Length", QByteArray::number(data.length()));
        socket->setHeader("Content-Type", "application/json; charset=utf-8");
        socket->write(data);
    }
    socket->close();
}

void QObjectHandlerPrivate::onReadChannelFinished()
{
    // Obtain the pointer to the socket emitting the signal
    QHttpSocket *socket = qobject_cast<QHttpSocket*>(sender());

    // Obtain the index and remove it from the map
    int index = map.take(socket);

    // Actually invoke the slot
    invokeSlot(socket, index);
}

QObjectHandler::QObjectHandler(QObject *parent)
    : QHttpHandler(parent), httpStatusCode(-1),
      d(new QObjectHandlerPrivate(this))
{
}

void QObjectHandler::process(QHttpSocket *socket, const QString &path)
{
    // Only GET | POST | PUT | DELETE requests are accepted - reject any other methods but ensure
    // that the Allow header is set in order to comply with RFC 2616
    if(socket->method() != QHttpSocket::HTTP_POST && socket->method() != QHttpSocket::HTTP_GET &&
       socket->method() != QHttpSocket::HTTP_DELETE && socket->method() != QHttpSocket::HTTP_PUT) {

        socket->setHeader("Allow", QString("%1, %2, %3, %4").arg(QHttpSocket::HTTP_GET,
                                                                 QHttpSocket::HTTP_PUT,
                                                                 QHttpSocket::HTTP_POST,
                                                                 QHttpSocket::HTTP_DELETE).toLocal8Bit());
        socket->writeError(QHttpSocket::MethodNotAllowed);
        return;
    }

    QString methodName;
    if(path.isEmpty()) {
        methodName = "http_" % socket->method().toLower();
    } else {
        methodName = socket->method().toLower() % "_" % path;
    }

    // Determine the index of the slot with the specified name - note that we
    // don't need to worry about retrieving the index for deleteLater() since
    // we specify the "QVariantMap" parameter type, which no parent slots use
    int index;

    if(socket->method() == QHttpSocket::HTTP_GET || socket->method() == QHttpSocket::HTTP_DELETE) {
        index = metaObject()->indexOfSlot(QString("%1()").arg(methodName).toUtf8().data());
    } else {
        if(socket->headers().contains("Content-Type") && socket->headers().value("Content-Type").startsWith("application/json")) {
            index = metaObject()->indexOfSlot(QString("%1(QVariantMap)").arg(methodName).toUtf8().data());
            if(index == -1)
                index = metaObject()->indexOfSlot(QString("%1(QByteArray)").arg(methodName).toUtf8().data());
        } else
            index = metaObject()->indexOfSlot(QString("%1(QByteArray)").arg(methodName).toUtf8().data());
    }
    // If the index is invalid, the "resource" was not found
    if(index == -1) {
        socket->writeError(QHttpSocket::NotFound);
        return;
    }

    // Ensure that the return type of the slot is QVariantMap
    QMetaMethod method = metaObject()->method(index);
    if(method.returnType() != QMetaType::QVariantMap) {
        qCritical()<< "Return type is not valid!!!";
        socket->writeError(QHttpSocket::InternalServerError);
        return;
    }

    // Check to see if the socket has finished receiving all of the data yet
    // or not - if so, jump to invokeSlot(), otherwise wait for the
    // readChannelFinished() signal
    if(socket->bytesAvailable() >= socket->contentLength()) {
        d->invokeSlot(socket, index);
    } else {

        // Add the socket and index to the map so that the latter can be
        // retrieved when the readChannelFinished() signal is emitted
        d->map.insert(socket, index);
        connect(socket, SIGNAL(readChannelFinished()), d, SLOT(onReadChannelFinished()));
    }
}
