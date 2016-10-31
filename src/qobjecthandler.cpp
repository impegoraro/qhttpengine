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
    QVariantMap retVal;
    QGenericReturnArgument ret;
    bool writeReturn = false;

    // Attempt to invoke the slot
    QMetaMethod wsMethod = q->metaObject()->method(index);

    if(wsMethod.returnType() != QMetaType::Void) {
        ret = Q_RETURN_ARG(QVariantMap, retVal);
        writeReturn = true;
    }

    if(socket->method() == QHttpSocket::HTTP_GET || socket->method() == QHttpSocket::HTTP_DELETE) {
        if(!wsMethod.invoke(q, ret, Q_ARG(QHttpSocket*, socket))) {
            socket->writeError(QHttpSocket::InternalServerError);
            return;
        }
    } else {
        if(wsMethod.parameterCount() == 1) {
            if(!wsMethod.invoke(q, ret, Q_ARG(QHttpSocket*, socket))) {
                socket->writeError(QHttpSocket::InternalServerError);
                return;
            }
        } else if(wsMethod.parameterCount() == 2) {
            QByteArray rawBody;
            QJsonDocument document;

            rawBody = socket->readAll();

            if(wsMethod.parameterType(1) == QMetaType::QByteArray) {
                if(!wsMethod.invoke(q, ret,
                                    Q_ARG(QHttpSocket*, socket),
                                    Q_ARG(QByteArray, rawBody))) {
                    socket->writeError(QHttpSocket::InternalServerError);
                    return;
                }
            } else {
                // Attempt to decode the JSON from the socket
                QJsonParseError error;
                document = QJsonDocument::fromJson(rawBody, &error);

                // Ensure that the document is valid
                if(error.error != QJsonParseError::NoError) {
                    socket->writeError(QHttpSocket::BadRequest);
                    return;
                }
                if(!wsMethod.invoke(q, ret,
                                    Q_ARG(QHttpSocket*, socket),
                                    Q_ARG(QVariantMap, document.object().toVariantMap()))) {
                    socket->writeError(QHttpSocket::InternalServerError);
                    return;
                }
            }

        } else {
            socket->writeError(QHttpSocket::InternalServerError);
            return ;
        }

    }

    if(writeReturn) {
        socket->writeHeaders();
        if(!retVal.isEmpty()) {
            QByteArray data = QJsonDocument(QJsonObject::fromVariantMap(retVal)).toJson();
            socket->setHeader("Content-Length", QByteArray::number(data.length()));
            socket->setHeader("Content-Type", "application/json; charset=utf-8");
            socket->write(data);
        }
    }
}

void QObjectHandlerPrivate::onReadChannelFinished()
{
    // Obtain the pointer to the socket emitting the signal
    QHttpSocket *socket = qobject_cast<QHttpSocket*>(sender());

    // Obtain the index and remove it from the map
    int index = map.take(socket);

    // Actually invoke the slot
    invokeSlot(socket, index);

    // We are done with the socket, lets close it
    socket->close();
}

QObjectHandler::QObjectHandler(QObject *parent)
    : QHttpHandler(parent),
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
        // We are done with the socket, lets close it
        socket->close();
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
    bool avoidReadAll = false;

    if(socket->method() == QHttpSocket::HTTP_GET || socket->method() == QHttpSocket::HTTP_DELETE) {
        index = metaObject()->indexOfSlot(QString("%1(QHttpSocket*)").arg(methodName).toUtf8().data());
    } else {
        if(socket->headers().contains("Content-Type") && socket->headers().value("Content-Type").startsWith("application/json")) {
            index = metaObject()->indexOfSlot(QString("%1(QHttpSocket*,QVariantMap)").arg(methodName).toUtf8().data());
            if(index == -1) {
                index = metaObject()->indexOfSlot(QString("%1(QHttpSocket*)").arg(methodName).toUtf8().data());
                avoidReadAll = true;
            }
        } else {
            index = metaObject()->indexOfSlot(QString("%1(QHttpSocket*,QByteArray)").arg(methodName).toUtf8().data());
            if(index == -1) {
                index = metaObject()->indexOfSlot(QString("%1(QHttpSocket*)").arg(methodName).toUtf8().data());
                avoidReadAll = true;
            }
        }
    }
    // If the index is invalid, the "resource" was not found
    if(index == -1) {
        socket->writeError(QHttpSocket::NotFound);
        // We are done with the socket, lets close it
        socket->close();
        return;
    }

    // Ensure that the return type of the slot is QVariantMap
    QMetaMethod method = metaObject()->method(index);
    if(method.returnType() != QMetaType::Void && method.returnType() != QMetaType::QVariantMap) {
        qCritical()<< "Return type is not valid!!!";
        socket->writeError(QHttpSocket::InternalServerError);
        // We are done with the socket, lets close it
        socket->close();
        return;
    }

    // Check to see if the socket has finished receiving all of the data yet
    // or not - if so, jump to invokeSlot(), otherwise wait for the
    // readChannelFinished() signal
    if(avoidReadAll || (socket->bytesAvailable() >= socket->contentLength())) {
        d->invokeSlot(socket, index);
        // We are done with the socket, lets close it
        socket->close();
    } else {

        // Add the socket and index to the map so that the latter can be
        // retrieved when the readChannelFinished() signal is emitted
        d->map.insert(socket, index);
        connect(socket, SIGNAL(readChannelFinished()), d, SLOT(onReadChannelFinished()));
    }
}
