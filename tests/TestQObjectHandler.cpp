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

#include <QObject>
#include <QTest>

#include <QHttpEngine/QHttpSocket>
#include <QHttpEngine/QObjectHandler>

#include "common/qsimplehttpclient.h"
#include "common/qsocketpair.h"

class DummyAPI : public QObject
{
    Q_OBJECT

public Q_SLOTS:

    void wrongArgumentCount() {}
    void wrongArgumentType(int) {}
    void valid(QHttpSocket *socket) {
        socket->writeError(QHttpSocket::OK);
    }
};

class TestQObjectHandler : public QObject
{
    Q_OBJECT

private Q_SLOTS:

    void testOldConnection_data();
    void testOldConnection();
    void testNewConnection();
};

void TestQObjectHandler::testOldConnection_data()
{
    QTest::addColumn<QByteArray>("slot");
    QTest::addColumn<int>("statusCode");

    QTest::newRow("invalid slot")
            << QByteArray(SLOT(invalid()))
            << static_cast<int>(QHttpSocket::InternalServerError);

    QTest::newRow("wrong argument count")
            << QByteArray(SLOT(wrongArgumentCount()))
            << static_cast<int>(QHttpSocket::InternalServerError);

    QTest::newRow("wrong argument type")
            << QByteArray(SLOT(wrongArgumentType(int)))
            << static_cast<int>(QHttpSocket::InternalServerError);

    QTest::newRow("valid")
            << QByteArray(SLOT(valid(QHttpSocket*)))
            << static_cast<int>(QHttpSocket::OK);
}

void TestQObjectHandler::testOldConnection()
{
    QFETCH(QByteArray, slot);
    QFETCH(int, statusCode);

    QObjectHandler handler;
    DummyAPI api;

    handler.registerMethod("test", &api, slot.constData());

    QSocketPair pair;
    QTRY_VERIFY(pair.isConnected());

    QSimpleHttpClient client(pair.client());
    QHttpSocket socket(pair.server(), &pair);

    client.sendHeaders("GET", "test");
    QTRY_VERIFY(socket.isHeadersParsed());

    handler.route(&socket, socket.path());
    QTRY_COMPARE(client.statusCode(), statusCode);
}

void TestQObjectHandler::testNewConnection()
{
    QObjectHandler handler;
    DummyAPI api;

    // Connect to object slot
    handler.registerMethod("0", &api, &DummyAPI::valid);

    // Connect to functor
    handler.registerMethod("1", [](QHttpSocket *socket) {
        socket->writeError(QHttpSocket::OK);
    });
    handler.registerMethod("2", &api, [](QHttpSocket *socket) {
        socket->writeError(QHttpSocket::OK);
    });

    for (int i = 0; i < 3; ++i) {
        QSocketPair pair;
        QTRY_VERIFY(pair.isConnected());

        QSimpleHttpClient client(pair.client());
        QHttpSocket socket(pair.server(), &pair);

        client.sendHeaders("GET", QByteArray::number(i));
        QTRY_VERIFY(socket.isHeadersParsed());

        handler.route(&socket, socket.path());
        QTRY_COMPARE(client.statusCode(), static_cast<int>(QHttpSocket::OK));
    }
}

QTEST_MAIN(TestQObjectHandler)
#include "TestQObjectHandler.moc"
