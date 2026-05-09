#include "signaling_client.h"

#include <nlohmann/json.hpp>

// 初始化信令客户端时只做信号绑定，不主动联网；真正连接由 connectToServer 触发。
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
    // Sender 侧始终通过单条 WebSocket 和服务端交互，所有会话消息都复用这条连接。
    qDebug() << "Connecting to signaling server:" << url;
    m_webSocket.open(QUrl(url));
}

void SignalingClient::onConnected()
{
    // 告知信令服务器当前连接可被分配为发送端。
    qDebug() << "Signaling connected!";
    QJsonObject json;
    json["type"] = "sender-ready";
    sendJson(json);
}

void SignalingClient::onTextMessageReceived(const QString &message)
{
    // 信令报文采用 JSON 文本协议，这里先解析出 type/id，再路由到具体的 WebRTC 会话。
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(message.toUtf8().constData());
    } catch (const std::exception& e) 
    {
        printf("JSON parse failed: %s\n", e.what());
        return;
    }

    // 每条消息至少依赖 type 和 id 完成路由；sdp/candidate 等字段按具体消息类型读取。
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
        // 接收端返回 answer 后，交给对应会话继续完成协商。
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
        // request 表示某个接收端请求开始一个新的桌面推流会话。
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

        // 必须先挂好回调，再执行 Init；否则首次 offer/candidate 可能在初始化过程中丢失。
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
        // 远端 ICE 候选按会话 id 转发到对应的 PeerConnection。
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
    // 绑定 WebRTC -> 信令通道：把底层协商事件封装成 JSON，再切回 Qt 线程发送。
    rtcClient->signaling.onLocalSdp = [this](const SdpBundle &bundle, std::string id)
    {
        // 该回调可能来自 WebRTC 内部线程，使用 invokeMethod 避免直接跨线程操作 QWebSocket。
        QJsonObject json;
        json["type"] = QString::fromStdString(bundle.type); // "offer"
        json["sdp"] = QString::fromStdString(bundle.sdp);
        json["id"] = QString::fromStdString(id);

        QMetaObject::invokeMethod(this, [this, json]()
                                  { sendJson(json); });
    };

    // 本地 ICE 候选收集到后同样转成信令消息发给接收端。
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
    // 统一使用紧凑 JSON，便于日志查看和浏览器端直接解析。
    if (m_webSocket.isValid())
    {
        QJsonDocument doc(json);
        m_webSocket.sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    }
}
