﻿/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtSerialBus module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL3$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPLv3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or later as published by the Free
** Software Foundation and appearing in the file LICENSE.GPL included in
** the packaging of this file. Please review the following information to
** ensure the GNU General Public License version 2.0 requirements will be
** met: http://www.gnu.org/licenses/gpl-2.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "socketcanbackend.h"

#include <QtCore/qdatastream.h>
#include <QtCore/qdebug.h>
#include <QtCore/qdiriterator.h>
#include <QtCore/qfile.h>
#include <QtCore/qsocketnotifier.h>

#include <linux/can/error.h>
#include <linux/can/raw.h>
#include <errno.h>
#include <unistd.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/time.h>

#ifndef CANFD_MTU
// CAN FD support was added by Linux kernel 3.6
// For prior kernels we redefine the missing defines here
// they are taken from linux/can/raw.h & linux/can.h

enum {
    CAN_RAW_FD_FRAMES = 5
};

#define CAN_MAX_DLEN 8
#define CANFD_MAX_DLEN 64
struct canfd_frame {
    canid_t can_id;  /* 32 bit CAN_ID + EFF/RTR/ERR flags */
    __u8    len;     /* frame payload length in byte */
    __u8    flags;   /* additional flags for CAN FD */
    __u8    __res0;  /* reserved / padding */
    __u8    __res1;  /* reserved / padding */
    __u8    data[CANFD_MAX_DLEN] __attribute__((aligned(8)));
};
#define CAN_MTU     (sizeof(struct can_frame))
#define CANFD_MTU   (sizeof(struct canfd_frame))

#endif

#ifndef CANFD_BRS
#   define CANFD_BRS 0x01 /* bit rate switch (second bitrate for payload data) */
#endif
#ifndef CANFD_ESI
#   define CANFD_ESI 0x02 /* error state indicator of the transmitting node */
#endif

QT_BEGIN_NAMESPACE

const char sysClassNetC[] = "/sys/class/net/";
const char flagsC[]       = "/flags";
const char mtuC[]         = "/mtu";
const char typeC[]        = "/type";
const char virtualC[]     = "virtual";

enum {
    CanFlexibleDataRateMtu = 72,
    TypeSocketCan = 280,
    DeviceIsActive = 1
};

static QByteArray fileContent(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();

    return file.readAll().trimmed();
}

static bool isFlexibleDataRateCapable(const QString &canDevice)
{
    const QString path = QLatin1String(sysClassNetC) + canDevice + QLatin1String(mtuC);
    const int mtu = fileContent(path).toInt();
    return mtu == CanFlexibleDataRateMtu;
}

static bool isVirtual(const QString &canDevice)
{
    const QFileInfo fi(QLatin1String(sysClassNetC) + canDevice);
    return fi.canonicalPath().contains(QLatin1String(virtualC));
}

static quint32 flags(const QString &canDevice)
{
    const QString path = QLatin1String(sysClassNetC) + canDevice + QLatin1String(flagsC);
    const quint32 result = fileContent(path).toUInt(nullptr, 0);
    return result;
}

QList<QCanBusDeviceInfo> SocketCanBackend::interfaces()
{
    QList<QCanBusDeviceInfo> result;
    QDirIterator it(sysClassNetC,
                    QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);

    while (it.hasNext()) {
        const QString dirEntry = it.next();
        if (fileContent(dirEntry + QLatin1String(typeC)).toInt() != TypeSocketCan)
            continue;

        const QString deviceName = dirEntry.mid(strlen(sysClassNetC));
        if (!(flags(deviceName) & DeviceIsActive))
            continue;

        auto info = createDeviceInfo(deviceName, isVirtual(deviceName),
                                     isFlexibleDataRateCapable(deviceName));
        result.append(info);
    }

    std::sort(result.begin(), result.end(),
              [](const QCanBusDeviceInfo &a, const QCanBusDeviceInfo &b) {
        return a.name() < b.name();
    });

    return result;
}

SocketCanBackend::SocketCanBackend(const QString &name) :
    canSocketName(name)
{
    resetConfigurations();
}

SocketCanBackend::~SocketCanBackend()
{
    close();
}

void SocketCanBackend::resetConfigurations()
{
    QCanBusDevice::setConfigurationParameter(
                QCanBusDevice::LoopbackKey, true);
    QCanBusDevice::setConfigurationParameter(
                QCanBusDevice::ReceiveOwnKey, false);
    QCanBusDevice::setConfigurationParameter(
                QCanBusDevice::ErrorFilterKey,
                QVariant::fromValue(QCanBusFrame::FrameErrors(QCanBusFrame::AnyError)));
    QCanBusDevice::setConfigurationParameter(
                QCanBusDevice::CanFdKey, false);
}

bool SocketCanBackend::open()
{
    if (canSocket == -1) {
        if (!connectSocket()) {
            close(); // sets UnconnectedState
            return false;
        }
    }

    setState(QCanBusDevice::ConnectedState);
    return true;
}

void SocketCanBackend::close()
{
    ::close(canSocket);
    canSocket = -1;

    setState(QCanBusDevice::UnconnectedState);
}

bool SocketCanBackend::applyConfigurationParameter(int key, const QVariant &value)
{
    bool success = false;

    switch (key) {
    case QCanBusDevice::LoopbackKey:
    {
        const int loopback = value.toBool() ? 1 : 0;
        if (Q_UNLIKELY(setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_LOOPBACK,
                                  &loopback, sizeof(loopback)) < 0)) {
            setError(qt_error_string(errno),
                     QCanBusDevice::CanBusError::ConfigurationError);
            break;
        }
        success = true;
        break;
    }
    case QCanBusDevice::ReceiveOwnKey:
    {
        const int receiveOwnMessages = value.toBool() ? 1 : 0;
        if (Q_UNLIKELY(setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS,
                                  &receiveOwnMessages, sizeof(receiveOwnMessages)) < 0)) {
            setError(qt_error_string(errno),
                     QCanBusDevice::CanBusError::ConfigurationError);
            break;
        }
        success = true;
        break;
    }
    case QCanBusDevice::ErrorFilterKey:
    {
        const int errorMask = value.value<QCanBusFrame::FrameErrors>();
        if (Q_UNLIKELY(setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
                                  &errorMask, sizeof(errorMask)) < 0)) {
            setError(qt_error_string(errno),
                     QCanBusDevice::CanBusError::ConfigurationError);
            break;
        }
        success = true;
        break;
    }
    case QCanBusDevice::RawFilterKey:
    {
        const QList<QCanBusDevice::Filter> filterList
                = value.value<QList<QCanBusDevice::Filter> >();
        if (!value.isValid() || filterList.isEmpty()) {
            // permit every frame - no restrictions (filter reset)
            can_filter filters = {0, 0};
            socklen_t s = sizeof(can_filter);
            if (Q_UNLIKELY(setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_FILTER,
                           &filters, s) != 0)) {
                qWarning("Cannot unset socket filters");
                setError(qt_error_string(errno),
                         QCanBusDevice::CanBusError::ConfigurationError);
                break;
            }
            success = true;
            break;
        }

        QVector<can_filter> filters;
        filters.resize(filterList.size());
        for (int i = 0; i < filterList.size(); i++) {
            const QCanBusDevice::Filter f = filterList.at(i);
            can_filter filter = { f.frameId, f.frameIdMask };

            // frame type filter
            switch (f.type) {
            default:
                // any other type cannot be filtered upon
                setError(tr("Cannot set filter for frame type: %1").arg(f.type),
                         QCanBusDevice::CanBusError::ConfigurationError);
                return false;
            case QCanBusFrame::InvalidFrame:
                break;
            case QCanBusFrame::DataFrame:
                filter.can_mask |= CAN_RTR_FLAG;
                break;
            case QCanBusFrame::ErrorFrame:
                filter.can_mask |= CAN_ERR_FLAG;
                filter.can_id |= CAN_ERR_FLAG;
                break;
            case QCanBusFrame::RemoteRequestFrame:
                filter.can_mask |= CAN_RTR_FLAG;
                filter.can_id |= CAN_RTR_FLAG;
                break;
            }

            // frame format filter
            if ((f.format & QCanBusDevice::Filter::MatchBaseAndExtendedFormat)
                    == QCanBusDevice::Filter::MatchBaseAndExtendedFormat) {
                // nothing
            } else if (f.format & QCanBusDevice::Filter::MatchBaseFormat) {
                filter.can_mask |= CAN_EFF_FLAG;
            } else if (f.format & QCanBusDevice::Filter::MatchExtendedFormat) {
                filter.can_mask |= CAN_EFF_FLAG;
                filter.can_id |= CAN_EFF_FLAG;
            }

            filters[i] = filter;
        }
        if (Q_UNLIKELY(setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_FILTER,
                       filters.constData(), sizeof(filters[0]) * filters.size()) < 0)) {
            setError(qt_error_string(errno),
                     QCanBusDevice::CanBusError::ConfigurationError);
            break;
        }
        success = true;
        break;
    }
    case QCanBusDevice::CanFdKey:
    {
        const int fd_frames = value.toBool() ? 1 : 0;
        if (Q_UNLIKELY(setsockopt(canSocket, SOL_CAN_RAW, CAN_RAW_FD_FRAMES,
                                  &fd_frames, sizeof(fd_frames)) < 0)) {
            setError(qt_error_string(errno),
                     QCanBusDevice::CanBusError::ConfigurationError);
            break;
        }
        success = true;
        break;
    }
    default:
        setError(tr("SocketCanBackend: No such configuration as %1 in SocketCanBackend").arg(key),
                 QCanBusDevice::CanBusError::ConfigurationError);
        break;
    }

    return success;
}

bool SocketCanBackend::connectSocket()
{
    struct ifreq interface;

    if (Q_UNLIKELY((canSocket = socket(PF_CAN, SOCK_RAW | SOCK_NONBLOCK, CAN_RAW)) < 0)) {
        setError(qt_error_string(errno),
                 QCanBusDevice::CanBusError::ConnectionError);
        return false;
    }

    qstrncpy(interface.ifr_name, canSocketName.toLatin1().constData(), sizeof(interface.ifr_name));
    if (Q_UNLIKELY(ioctl(canSocket, SIOCGIFINDEX, &interface) < 0)) {
        setError(qt_error_string(errno),
                 QCanBusDevice::CanBusError::ConnectionError);
        return false;
    }

    m_address.can_family  = AF_CAN;
    m_address.can_ifindex = interface.ifr_ifindex;

    if (Q_UNLIKELY(bind(canSocket, reinterpret_cast<struct sockaddr *>(&m_address), sizeof(m_address)) < 0)) {
        setError(qt_error_string(errno),
                 QCanBusDevice::CanBusError::ConnectionError);
        return false;
    }

    m_iov.iov_base = &m_frame;
    m_msg.msg_name = &m_address;
    m_msg.msg_iov = &m_iov;
    m_msg.msg_iovlen = 1;
    m_msg.msg_control = &m_ctrlmsg;

    delete notifier;

    notifier = new QSocketNotifier(canSocket, QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated,
            this, &SocketCanBackend::readSocket);

    //apply all stored configurations
    const auto keys = configurationKeys();
    for (int key : keys) {
        const QVariant param = configurationParameter(key);
        bool success = applyConfigurationParameter(key, param);
        if (Q_UNLIKELY(!success)) {
            qWarning("Cannot apply parameter: %d with value: %ls",
                     key, qUtf16Printable(param.toString()));
        }
    }

    return true;
}

void SocketCanBackend::setConfigurationParameter(int key, const QVariant &value)
{
    if (key == QCanBusDevice::RawFilterKey) {
        //verify valid/supported filters

        const auto filters = value.value<QList<QCanBusDevice::Filter> >();
        for (QCanBusDevice::Filter f : filters) {
            switch (f.type) {
            case QCanBusFrame::UnknownFrame:

            default:
                setError(tr("Cannot set filter for frame type: %1").arg(f.type),
                         QCanBusDevice::CanBusError::ConfigurationError);
                return;
            case QCanBusFrame::InvalidFrame:
            case QCanBusFrame::DataFrame:
            case QCanBusFrame::ErrorFrame:
            case QCanBusFrame::RemoteRequestFrame:
                break;
            }

            if (f.frameId > 0x1FFFFFFFU) {
                setError(tr("FrameId %1 larger than 29 bit.").arg(f.frameId),
                         QCanBusDevice::CanBusError::ConfigurationError);
                return;
            }
        }
    }
    // connected & params not applyable/invalid
    if (canSocket != -1 && !applyConfigurationParameter(key, value))
        return;

    QCanBusDevice::setConfigurationParameter(key, value);

    // we need to check CAN FD option a lot -> cache it and avoid QVector lookup
    if (key == QCanBusDevice::CanFdKey)
        canFdOptionEnabled = value.toBool();
}

bool SocketCanBackend::writeFrame(const QCanBusFrame &newData)
{
    if (state() != ConnectedState)
        return false;

    if (Q_UNLIKELY(!newData.isValid())) {
        setError(tr("Cannot write invalid QCanBusFrame"), QCanBusDevice::WriteError);
        return false;
    }

    canid_t canId = newData.frameId();
    if (newData.hasExtendedFrameFormat())
        canId |= CAN_EFF_FLAG;

    if (newData.frameType() == QCanBusFrame::RemoteRequestFrame) {
        canId |= CAN_RTR_FLAG;
    } else if (newData.frameType() == QCanBusFrame::ErrorFrame) {
        canId = static_cast<canid_t>((newData.error() & QCanBusFrame::AnyError));
        canId |= CAN_ERR_FLAG;
    }

    if (Q_UNLIKELY(!canFdOptionEnabled && newData.hasFlexibleDataRateFormat())) {
        const QString error = tr("Cannot write CAN FD frame because CAN FD option is not enabled.");
        qDebug("%ls", qUtf16Printable(error));
        setError(error, QCanBusDevice::WriteError);
        return false;
    }

    qint64 bytesWritten = 0;
    if (newData.hasFlexibleDataRateFormat()) {
        canfd_frame frame;
        ::memset(&frame, 0, sizeof(frame));
        frame.len = newData.payload().size();
        frame.can_id = canId;
        frame.flags = newData.hasBitrateSwitch() ? CANFD_BRS : 0;
        frame.flags |= newData.hasErrorStateIndicator() ? CANFD_ESI : 0;
        ::memcpy(frame.data, newData.payload().constData(), frame.len);

        bytesWritten = ::write(canSocket, &frame, sizeof(frame));
    } else {
        can_frame frame;
        ::memset(&frame, 0, sizeof(frame));
        frame.can_dlc = newData.payload().size();
        frame.can_id = canId;
        ::memcpy(frame.data, newData.payload().constData(), frame.can_dlc);

        bytesWritten = ::write(canSocket, &frame, sizeof(frame));
    }

    if (Q_UNLIKELY(bytesWritten < 0)) {
        setError(qt_error_string(errno),
                 QCanBusDevice::CanBusError::WriteError);
        return false;
    }

    emit framesWritten(1);

    return true;
}

QString SocketCanBackend::interpretErrorFrame(const QCanBusFrame &errorFrame)
{
    if (errorFrame.frameType() != QCanBusFrame::ErrorFrame)
        return QString();

    // the payload may contain the error details
    const QByteArray data = errorFrame.payload();
    QString errorMsg;

    if (errorFrame.error() & QCanBusFrame::TransmissionTimeoutError)
        errorMsg += QStringLiteral("TX timout\n");

    if (errorFrame.error() & QCanBusFrame::MissingAcknowledgmentError)
        errorMsg += QStringLiteral("Received no ACK on transmission\n");

    if (errorFrame.error() & QCanBusFrame::BusOffError)
        errorMsg += QStringLiteral("Bus off\n");

    if (errorFrame.error() & QCanBusFrame::BusError)
        errorMsg += QStringLiteral("Bus error\n");

    if (errorFrame.error() & QCanBusFrame::ControllerRestartError)
        errorMsg += QStringLiteral("Controller restarted\n");

    if (errorFrame.error() & QCanBusFrame::UnknownError)
        errorMsg += QStringLiteral("Unknown error\n");

    if (errorFrame.error() & QCanBusFrame::LostArbitrationError) {
        errorMsg += QStringLiteral("Lost arbitration:\n");
        if (data.size() >= 1) {
            errorMsg += QString::number(data.at(0), 16);
            errorMsg += QStringLiteral(" bit\n");
        }
    }

    if (errorFrame.error() & QCanBusFrame::ControllerError) {
        errorMsg += QStringLiteral("Controller problem:\n");
        if (data.size() >= 2) {
            char b = data.at(1) ;
            if (b & CAN_ERR_CRTL_RX_OVERFLOW)
                errorMsg += QStringLiteral(" RX buffer overflow\n");
            if (b & CAN_ERR_CRTL_TX_OVERFLOW)
                errorMsg += QStringLiteral(" TX buffer overflow\n");
            if (b & CAN_ERR_CRTL_RX_WARNING)
                errorMsg += QStringLiteral(" reached warning level for RX errors\n");
            if (b & CAN_ERR_CRTL_TX_WARNING)
                errorMsg += QStringLiteral(" reached warning level for TX errors\n");
            if (b & CAN_ERR_CRTL_RX_PASSIVE)
                errorMsg += QStringLiteral(" reached error passive status RX\n");
            if (b & CAN_ERR_CRTL_TX_PASSIVE)
                errorMsg += QStringLiteral(" reached error passive status TX\n");

            if (b == CAN_ERR_CRTL_UNSPEC)
                errorMsg += QStringLiteral(" Unspecified error\n");
        }
    }

    if (errorFrame.error() & QCanBusFrame::TransceiverError) {
        errorMsg = QStringLiteral("Transceiver status:");
        if (data.size() >= 5) {
            char b = data.at(4);
            if (b & CAN_ERR_TRX_CANH_NO_WIRE)
                errorMsg += QStringLiteral(" CAN-transceiver CANH no wire\n");
            if (b & CAN_ERR_TRX_CANH_SHORT_TO_BAT)
                errorMsg += QStringLiteral(" CAN-transceiver CANH short to bat\n");
            if (b & CAN_ERR_TRX_CANH_SHORT_TO_VCC)
                errorMsg += QStringLiteral(" CAN-transceiver CANH short to vcc\n");
            if (b & CAN_ERR_TRX_CANH_SHORT_TO_GND)
                errorMsg += QStringLiteral(" CAN-transceiver CANH short to ground\n");
            if (b & CAN_ERR_TRX_CANL_NO_WIRE)
                errorMsg += QStringLiteral(" CAN-transceiver CANL no wire\n");
            if (b & CAN_ERR_TRX_CANL_SHORT_TO_BAT)
                errorMsg += QStringLiteral(" CAN-transceiver CANL short to bat\n");
            if (b & CAN_ERR_TRX_CANL_SHORT_TO_VCC)
                errorMsg += QStringLiteral(" CAN-transceiver CANL short to vcc\n");
            if (b & CAN_ERR_TRX_CANL_SHORT_TO_GND)
                errorMsg += QStringLiteral(" CAN-transceiver CANL short to ground\n");
            if (b & CAN_ERR_TRX_CANL_SHORT_TO_CANH)
                errorMsg += QStringLiteral(" CAN-transceiver CANL short to CANH\n");

            if (b == CAN_ERR_TRX_UNSPEC)
                errorMsg += QStringLiteral(" unspecified\n");
        }

    }

    if (errorFrame.error() & QCanBusFrame::ProtocolViolationError) {
        errorMsg += QStringLiteral("Protocol violation:\n");
        if (data.size() > 3) {
            char b = data.at(2);
            if (b & CAN_ERR_PROT_BIT)
                errorMsg += QStringLiteral(" single bit error\n");
            if (b & CAN_ERR_PROT_FORM)
                errorMsg += QStringLiteral(" frame format error\n");
            if (b & CAN_ERR_PROT_STUFF)
                errorMsg += QStringLiteral(" bit stuffing error\n");
            if (b & CAN_ERR_PROT_BIT0)
                errorMsg += QStringLiteral(" unable to send dominant bit\n");
            if (b & CAN_ERR_PROT_BIT1)
                errorMsg += QStringLiteral(" unable to send recessive bit\n");
            if (b & CAN_ERR_PROT_OVERLOAD)
                errorMsg += QStringLiteral(" bus overload\n");
            if (b & CAN_ERR_PROT_ACTIVE)
                errorMsg += QStringLiteral(" active error announcement\n");
            if (b & CAN_ERR_PROT_TX)
                errorMsg += QStringLiteral(" error occurred on transmission\n");

            if (b == CAN_ERR_PROT_UNSPEC)
                errorMsg += QStringLiteral(" unspecified\n");
        }
        if (data.size() > 4) {
            char b = data.at(3);
            if (b == CAN_ERR_PROT_LOC_SOF)
                errorMsg += QStringLiteral(" start of frame\n");
            if (b == CAN_ERR_PROT_LOC_ID28_21)
                errorMsg += QStringLiteral(" ID bits 28 - 21 (SFF: 10 - 3)\n");
            if (b == CAN_ERR_PROT_LOC_ID20_18)
                errorMsg += QStringLiteral(" ID bits 20 - 18 (SFF: 2 - 0 )\n");
            if (b == CAN_ERR_PROT_LOC_SRTR)
                errorMsg += QStringLiteral(" substitute RTR (SFF: RTR)\n");
            if (b == CAN_ERR_PROT_LOC_IDE)
                errorMsg += QStringLiteral(" identifier extension\n");
            if (b == CAN_ERR_PROT_LOC_ID17_13)
                errorMsg += QStringLiteral(" ID bits 17-13\n");
            if (b == CAN_ERR_PROT_LOC_ID12_05)
                errorMsg += QStringLiteral(" ID bits 12-5\n");
            if (b == CAN_ERR_PROT_LOC_ID04_00)
                errorMsg += QStringLiteral(" ID bits 4-0\n");
            if (b == CAN_ERR_PROT_LOC_RTR)
                errorMsg += QStringLiteral(" RTR\n");
            if (b == CAN_ERR_PROT_LOC_RES1)
                errorMsg += QStringLiteral(" reserved bit 1\n");
            if (b == CAN_ERR_PROT_LOC_RES0)
                errorMsg += QStringLiteral(" reserved bit 0\n");
            if (b == CAN_ERR_PROT_LOC_DLC)
                errorMsg += QStringLiteral(" data length code\n");
            if (b == CAN_ERR_PROT_LOC_DATA)
                errorMsg += QStringLiteral(" data section\n");
            if (b == CAN_ERR_PROT_LOC_CRC_SEQ)
                errorMsg += QStringLiteral(" CRC sequence\n");
            if (b == CAN_ERR_PROT_LOC_CRC_DEL)
                errorMsg += QStringLiteral(" CRC delimiter\n");
            if (b == CAN_ERR_PROT_LOC_ACK)
                errorMsg += QStringLiteral(" ACK slot\n");
            if (b == CAN_ERR_PROT_LOC_ACK_DEL)
                errorMsg += QStringLiteral(" ACK delimiter\n");
            if (b == CAN_ERR_PROT_LOC_EOF)
                errorMsg += QStringLiteral(" end of frame\n");
            if (b == CAN_ERR_PROT_LOC_INTERM)
                errorMsg += QStringLiteral(" Intermission\n");

            if (b == CAN_ERR_PROT_LOC_UNSPEC)
                errorMsg += QStringLiteral(" unspecified\n");
        }
    }

    // cut trailing \n
    if (!errorMsg.isEmpty())
        errorMsg.chop(1);

    return errorMsg;
}

void SocketCanBackend::readSocket()
{
    QVector<QCanBusFrame> newFrames;

    for (;;) {
        ::memset(&m_frame, 0, sizeof(m_frame));
        m_iov.iov_len = sizeof(m_frame);
        m_msg.msg_namelen = sizeof(m_addr);
        m_msg.msg_controllen = sizeof(m_ctrlmsg);
        m_msg.msg_flags = 0;

        const int bytesReceived = ::recvmsg(canSocket, &m_msg, 0);

        if (bytesReceived <= 0) {
            break;
        } else if (Q_UNLIKELY(bytesReceived != CANFD_MTU && bytesReceived != CAN_MTU)) {
            setError(tr("ERROR SocketCanBackend: incomplete CAN frame"),
                     QCanBusDevice::CanBusError::ReadError);
            continue;
        } else if (Q_UNLIKELY(m_frame.len > bytesReceived - offsetof(canfd_frame, data))) {
            setError(tr("ERROR SocketCanBackend: invalid CAN frame length"),
                     QCanBusDevice::CanBusError::ReadError);
            continue;
        }

        struct timeval timeStamp;
        if (Q_UNLIKELY(ioctl(canSocket, SIOCGSTAMP, &timeStamp) < 0)) {
            setError(qt_error_string(errno),
                     QCanBusDevice::CanBusError::ReadError);
            ::memset(&timeStamp, 0, sizeof(timeStamp));
        }

        const QCanBusFrame::TimeStamp stamp(timeStamp.tv_sec, timeStamp.tv_usec);
        QCanBusFrame bufferedFrame;
        bufferedFrame.setTimeStamp(stamp);
        bufferedFrame.setFlexibleDataRateFormat(bytesReceived == CANFD_MTU);

        bufferedFrame.setExtendedFrameFormat(m_frame.can_id & CAN_EFF_FLAG);
        Q_ASSERT(m_frame.len <= CANFD_MAX_DLEN);

        if (m_frame.can_id & CAN_RTR_FLAG)
            bufferedFrame.setFrameType(QCanBusFrame::RemoteRequestFrame);
        if (m_frame.can_id & CAN_ERR_FLAG)
            bufferedFrame.setFrameType(QCanBusFrame::ErrorFrame);
        if (m_frame.flags & CANFD_BRS)
            bufferedFrame.setBitrateSwitch(true);
        if (m_frame.flags & CANFD_ESI)
            bufferedFrame.setErrorStateIndicator(true);
        if (m_msg.msg_flags & MSG_CONFIRM)
            bufferedFrame.setLocalEcho(true);

        bufferedFrame.setFrameId(m_frame.can_id & CAN_EFF_MASK);

        const QByteArray load(reinterpret_cast<char *>(m_frame.data), m_frame.len);
        bufferedFrame.setPayload(load);

        newFrames.append(std::move(bufferedFrame));
    }

    enqueueReceivedFrames(newFrames);
}

QT_END_NAMESPACE
