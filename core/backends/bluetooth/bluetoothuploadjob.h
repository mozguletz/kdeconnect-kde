/*
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

#ifndef BLUETOOTHUPLOADJOB_H
#define BLUETOOTHUPLOADJOB_H

#include <QIODevice>
#include <QThread>
#include <QVariantMap>
#include <QSharedPointer>
#include <QBluetoothAddress>
#include <QBluetoothUuid>
#include <QBluetoothServer>

class BluetoothUploadJob
    : public QObject
{
    Q_OBJECT
public:
    explicit BluetoothUploadJob(const QSharedPointer<QIODevice> &data, const QBluetoothAddress &remoteAddress, QObject* parent = 0);

    QVariantMap transferInfo() const;
    void start();

private:
    QSharedPointer<QIODevice> mData;
    QBluetoothAddress mRemoteAddress;
    QBluetoothUuid mTransferUuid;
    QBluetoothServer* mServer;
    QBluetoothServiceInfo mServiceInfo;

private Q_SLOTS:
    void newConnection();
};

#endif // BLUETOOTHUPLOADJOB_H
