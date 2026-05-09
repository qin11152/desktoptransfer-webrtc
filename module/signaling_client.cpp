#include "signaling_client.h"

#include <nlohmann/json.hpp>

SignalingClient::SignalingClient(QObject *parent)
    : QObject(parent)
{
    connect(&m_webSocket, &QWebSocket::connected, this, &SignalingClient::onConnected);
    connect(&m_webSocket, &QWebSocket::textMessageReceived, this, &SignalingClient::onTextMessageReceived);
    connect(&m_webSocket, &QWebSocket::disconnected, this, &SignalingClient::onDisconnected);

    // setupCallbacks();
}

void SignalingClient::connectToServer(const QString &url)
{
    qDebug() << "Connecting to signaling server:" << url;
    m_webSocket.open(QUrl(url));
}

void SignalingClient::onConnected()
{
    qDebug() << "Signaling connected!";
    QJsonObject json;
    json["type"] = "sender-ready";
    sendJson(json);
}

void SignalingClient::onTextMessageReceived(const QString &message)
{

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(message.toUtf8().constData());
    } catch (const std::exception& e) 
    {
        printf("JSON parse failed: %s\n", e.what());
        return;
    }

    // 2. 取出 type 和 sdp
    std::string type = j.value("type", "");
    std::string sdp  = j.value("sdp", "");
    std::string id   = j.value("id", "");

    printf("Remote SDP type: %s\n", type.c_str());

    // printf("Signaling message received: %s\n", message.toUtf8().constData());
    // QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    // if (doc.isNull() || !doc.isObject())
    //     return;

    // QJsonObject json = doc.object();
    // QString type = json["type"].toString();

    // auto id = json["id"].toString();
    // qDebug() << "Received message of type:" << type << "from id:" << id;

    if (type == "answer")
    {
        std::string sdp = j.value("sdp", "");
        auto jt = clients.find(id);
        if (jt != clients.end())
        {
            jt->second->SetRemoteAnswer(sdp);
        }
        // m_rtcClient->SetRemoteAnswer(sdp.toStdString());
    }
    else if (type == "request")
    {
        if (!WebRTCPushClient::IsDesktopCaptureAvailable())
        {
            QJsonObject json;
            json["type"] = "error";
            json["id"] = QString::fromStdString(id);
            json["message"] = QStringLiteral("desktop capture is unavailable in the current environment or this build lacks the required backend");
            sendJson(json);
            return;
        }

        auto client = std::make_shared<WebRTCPushClient>(id);
        std::vector<IceServerConfig> iceServers = {
            {"stun:stun.l.google.com:19302", "", ""}};
        setupCallbacks(client);
        if (!client->Init(iceServers))
        {
            QJsonObject json;
            json["type"] = "error";
            json["id"] = QString::fromStdString(id);
            json["message"] = QStringLiteral("native sender failed to initialize desktop capture in the current environment");
            sendJson(json);
            return;
        }

        clients[id] = std::move(client);
    }
    else if (type == "candidate")
    {
        std::string candidate = j.value("candidate", "");
        std::string sdpMid = j.value("sdpMid", "");
        int sdpMLineIndex = j.value("sdpMLineIndex", 0);

        qDebug() << "Received Remote ICE";
        auto jt = clients.find(id);
        if (jt == clients.end())
        {
            qWarning() << "Ignore ICE for unknown client id:" << QString::fromStdString(id);
            return;
        }

        jt->second->AddRemoteIce(candidate, sdpMLineIndex, sdpMid);
    }
}

void SignalingClient::onDisconnected()
{
    qDebug() << "Signaling disconnected!";
}

void SignalingClient::setupCallbacks(std::shared_ptr<WebRTCPushClient> rtcClient)
{
    // 1. 当 WebRTC 生成本地 Offer 时，通过 WebSocket 发送
    rtcClient->signaling.onLocalSdp = [this](const SdpBundle &bundle, std::string id)
    {
        // 注意：这里是在 WebRTC 线程回调的，建议通过 Qt 的信号槽或 invokeMethod 转到主线程发送
        // 简单起见，QWebSocket 是线程安全的（write 操作），但最好用 QMetaObject::invokeMethod
        QJsonObject json;
        json["type"] = QString::fromStdString(bundle.type); // "offer"
        json["sdp"] = QString::fromStdString(bundle.sdp);
        json["id"] = QString::fromStdString(id);

        QMetaObject::invokeMethod(this, [this, json]()
                                  { sendJson(json); });
    };

    // 2. 当 WebRTC 收集到本地 ICE 时，通过 WebSocket 发送
    rtcClient->signaling.onLocalIce = [this](const IceCandidateBundle &ice)
    {
        QJsonObject json;
        json["type"] = "candidate";
        json["candidate"] = QString::fromStdString(ice.candidate);
        json["sdpMid"] = QString::fromStdString(ice.sdp_mid);
        json["sdpMLineIndex"] = ice.sdp_mline_index;
        json["id"] = QString::fromStdString(ice.id);

        QMetaObject::invokeMethod(this, [this, json]()
                                  { sendJson(json); });
    };
}

void SignalingClient::sendJson(const QJsonObject &json)
{
    if (m_webSocket.isValid())
    {
        QJsonDocument doc(json);
        m_webSocket.sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    }
}
