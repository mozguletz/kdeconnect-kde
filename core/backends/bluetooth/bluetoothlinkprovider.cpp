/**
 * Copyright 2016 Saikrishna Arcot <saiarcot895@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License or (at your option) version 3 or any later version
 * accepted by the membership of KDE e.V. (or its successor approved
 * by the membership of KDE e.V.), which shall act as a proxy
 * defined in Section 14 of version 3 of the license.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "bluetoothlinkprovider.h"
#include "core_debug.h"

#include <KSharedConfig>
#include <KConfigGroup>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QBluetoothServiceInfo>

#include "bluetoothdevicelink.h"

BluetoothLinkProvider::BluetoothLinkProvider()
{
    mServiceUuid = QBluetoothUuid(QString("185f3df4-3268-4e3f-9fca-d4d5059915bd"));

    connectTimer = new QTimer(this);
    connectTimer->setInterval(30000);
    connectTimer->setSingleShot(false);

    mServiceDiscoveryAgent = new QBluetoothServiceDiscoveryAgent(this);
    mServiceDiscoveryAgent->setUuidFilter(mServiceUuid);
    connect(mServiceDiscoveryAgent, SIGNAL(finished()), this, SLOT(serviceDiscoveryFinished()));
    connect(connectTimer, SIGNAL(timeout()), mServiceDiscoveryAgent, SLOT(start()));
}

void BluetoothLinkProvider::onStart()
{
    QBluetoothLocalDevice localDevice;
    if (!localDevice.isValid()) {
        qCWarning(KDECONNECT_CORE) << "No local bluetooth adapter found";
        return;
    }

    mBluetoothServer = new QBluetoothServer(QBluetoothServiceInfo::RfcommProtocol, this);
    mBluetoothServer->setSecurityFlags(QBluetooth::Encryption | QBluetooth::Secure);
    connect(mBluetoothServer, SIGNAL(newConnection()), this, SLOT(serverNewConnection()));

    mServiceDiscoveryAgent->start();

    connectTimer->start();
    mKdeconnectService = mBluetoothServer->listen(mServiceUuid, "KDE Connect");
}

void BluetoothLinkProvider::onStop()
{
    if (!mBluetoothServer) {
        return;
    }

    connectTimer->stop();

    mKdeconnectService.unregisterService();
    mBluetoothServer->close();
    mBluetoothServer->deleteLater();
}

//I'm in a new network, let's be polite and introduce myself
void BluetoothLinkProvider::onNetworkChange()
{
}

void BluetoothLinkProvider::connectError()
{
    QBluetoothSocket* socket = qobject_cast<QBluetoothSocket*>(sender());
    if (!socket) return;

    qCWarning(KDECONNECT_CORE) << "Couldn't connect to socket:" << socket->errorString();

    disconnect(socket, SIGNAL(connected()), this, SLOT(clientConnected()));
    disconnect(socket, SIGNAL(readyRead()), this, SLOT(serverDataReceived()));
    disconnect(socket, SIGNAL(error(QBluetoothSocket::SocketError)), this, SLOT(connectError()));

    mSockets.remove(socket->peerAddress());
    socket->deleteLater();
}

void BluetoothLinkProvider::addLink(BluetoothDeviceLink* deviceLink, const QString& deviceId)
{
    QMap< QString, DeviceLink* >::iterator oldLinkIterator = mLinks.find(deviceId);
    if (oldLinkIterator != mLinks.end()) {
        DeviceLink* oldLink = oldLinkIterator.value();
        disconnect(oldLink, SIGNAL(destroyed(QObject*)),
                    this, SLOT(deviceLinkDestroyed(QObject*)));
        oldLink->deleteLater();
        mLinks.erase(oldLinkIterator);
    }

    mLinks[deviceId] = deviceLink;
}

//I'm the new device and I found an existing device
void BluetoothLinkProvider::serviceDiscoveryFinished()
{
    QList<QBluetoothServiceInfo> services = mServiceDiscoveryAgent->discoveredServices();

    Q_FOREACH (QBluetoothServiceInfo info, services) {
        if (mSockets.contains(info.device().address())) {
            continue;
        }

        QBluetoothSocket* socket = new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol);

        connect(socket, SIGNAL(connected()), this, SLOT(clientConnected()));
        connect(socket, SIGNAL(error(QBluetoothSocket::SocketError)), this, SLOT(connectError()));

        socket->connectToService(info);

        qCDebug(KDECONNECT_CORE()) << "Connecting to" << info.device().address();

        if (socket->error() != QBluetoothSocket::NoSocketError) {
            qCWarning(KDECONNECT_CORE) << "Socket connection error:" << socket->errorString();
        }
    }
}

//I'm the new device and I'm connected to the existing device. Time to get data.
void BluetoothLinkProvider::clientConnected()
{
    QBluetoothSocket* socket = qobject_cast<QBluetoothSocket*>(sender());
    if (!socket) return;

    qCDebug(KDECONNECT_CORE()) << "Connected to" << socket->peerAddress();

    if (mSockets.contains(socket->peerAddress())) {
        qCWarning(KDECONNECT_CORE()) << "Duplicate connection to" << socket->peerAddress();
        socket->close();
        socket->deleteLater();
        return;
    }

    mSockets.insert(socket->peerAddress(), socket);

    disconnect(socket, SIGNAL(connected()), this, SLOT(clientConnected()));

    connect(socket, SIGNAL(readyRead()), this, SLOT(clientIdentityReceived()));
    connect(socket, SIGNAL(disconnected()), this, SLOT(socketDisconnected()));
}

//I'm the new device and the existing device sent me data.
void BluetoothLinkProvider::clientIdentityReceived()
{
    QBluetoothSocket* socket = qobject_cast<QBluetoothSocket*>(sender());
    if (!socket) return;

    QByteArray identityArray;
    while (socket->bytesAvailable() > 0 || !identityArray.contains('\n')) {
        identityArray += socket->readAll();
    }

    disconnect(socket, SIGNAL(readyRead()), this, SLOT(clientIdentityReceived()));

    NetworkPackage receivedPackage("");
    bool success = NetworkPackage::unserialize(identityArray, &receivedPackage);

    if (!success || receivedPackage.type() != PACKAGE_TYPE_IDENTITY) {
        qCWarning(KDECONNECT_CORE) << "Not an identity package";
        mSockets.remove(socket->peerAddress());
        socket->close();
        socket->deleteLater();
        return;
    }

    qCDebug(KDECONNECT_CORE()) << "Received identity package from" << socket->peerAddress();

    disconnect(socket, SIGNAL(error(QBluetoothSocket::SocketError)), this, SLOT(connectError()));

    const QString& deviceId = receivedPackage.get<QString>("deviceId");
    BluetoothDeviceLink* deviceLink = new BluetoothDeviceLink(deviceId, this, socket);

    NetworkPackage np2("");
    NetworkPackage::createIdentityPackage(&np2);
    success = deviceLink->sendPackage(np2);

    if (success) {

        qCDebug(KDECONNECT_CORE) << "Handshaking done (I'm the new device)";

        connect(deviceLink, SIGNAL(destroyed(QObject*)),
                this, SLOT(deviceLinkDestroyed(QObject*)));

        Q_EMIT onConnectionReceived(receivedPackage, deviceLink);

        //We kill any possible link from this same device
        addLink(deviceLink, deviceId);

    } else {
        // Connection might be lost. Delete it.
        delete deviceLink;
    }

    //We don't delete the socket because now it's owned by the BluetoothDeviceLink
}

//I'm the existing device, a new device is kindly introducing itself.
void BluetoothLinkProvider::serverNewConnection()
{
    QBluetoothSocket* socket = mBluetoothServer->nextPendingConnection();

    qCDebug(KDECONNECT_CORE()) << "Received connection from" << socket->peerAddress();

    if (mSockets.contains(socket->peerAddress())) {
        qCDebug(KDECONNECT_CORE()) << "Duplicate connection from" << socket->peerAddress();
        socket->close();
        socket->deleteLater();
        return;
    }

    connect(socket, SIGNAL(readyRead()), this, SLOT(serverDataReceived()));
    connect(socket, SIGNAL(error(QBluetoothSocket::SocketError)), this, SLOT(connectError()));
    connect(socket, SIGNAL(disconnected()), this, SLOT(socketDisconnected()));

    NetworkPackage np2("");
    NetworkPackage::createIdentityPackage(&np2);
    socket->write(np2.serialize());

    qCDebug(KDECONNECT_CORE()) << "Sent identity package to" << socket->peerAddress();

    mSockets.insert(socket->peerAddress(), socket);
}

//I'm the existing device and this is the answer to my identity package (data received)
void BluetoothLinkProvider::serverDataReceived()
{
    QBluetoothSocket* socket = qobject_cast<QBluetoothSocket*>(sender());
    if (!socket) return;

    QByteArray identityArray;
    while (socket->bytesAvailable() > 0 || !identityArray.contains('\n')) {
        identityArray += socket->readAll();
    }

    disconnect(socket, SIGNAL(readyRead()), this, SLOT(serverDataReceived()));
    disconnect(socket, SIGNAL(error(QBluetoothSocket::SocketError)), this, SLOT(connectError()));

    NetworkPackage receivedPackage("");
    bool success = NetworkPackage::unserialize(identityArray, &receivedPackage);

    if (!success || receivedPackage.type() != PACKAGE_TYPE_IDENTITY) {
        qCWarning(KDECONNECT_CORE) << "Not an identity package.";
        mSockets.remove(socket->peerAddress());
        socket->close();
        socket->deleteLater();
        return;
    }

    qCDebug(KDECONNECT_CORE()) << "Received identity package from" << socket->peerAddress();

    const QString& deviceId = receivedPackage.get<QString>("deviceId");
    BluetoothDeviceLink* deviceLink = new BluetoothDeviceLink(deviceId, this, socket);

    connect(deviceLink, SIGNAL(destroyed(QObject*)),
            this, SLOT(deviceLinkDestroyed(QObject*)));

    Q_EMIT onConnectionReceived(receivedPackage, deviceLink);

    addLink(deviceLink, deviceId);
}

void BluetoothLinkProvider::deviceLinkDestroyed(QObject* destroyedDeviceLink)
{
    const QString id = destroyedDeviceLink->property("deviceId").toString();
    qCDebug(KDECONNECT_CORE()) << "Device disconnected:" << id;
    QMap< QString, DeviceLink* >::iterator oldLinkIterator = mLinks.find(id);
    if (oldLinkIterator != mLinks.end() && oldLinkIterator.value() == destroyedDeviceLink) {
        mLinks.erase(oldLinkIterator);
    }
}

void BluetoothLinkProvider::socketDisconnected()
{
    QBluetoothSocket* socket = qobject_cast<QBluetoothSocket*>(sender());
    if (!socket) return;

    disconnect(socket, SIGNAL(disconnected()), this, SLOT(socketDisconnected()));

    mSockets.remove(mSockets.key(socket));
}

BluetoothLinkProvider::~BluetoothLinkProvider()
{

}
