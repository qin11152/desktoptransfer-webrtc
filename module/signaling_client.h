#pragma once

#include <QObject>
#include <QWebSocket>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJsonArray>
#include <QUrl>
#include <string>
#include <unordered_map>
#include <memory>
#include "pushclient.h"

// Sender 侧信令控制器：
// 1. 连接信令服务器并声明自身为 sender-ready。
// 2. 接收 request/answer/candidate 等消息。
// 3. 为每个会话 id 维护一个独立的 WebRTCPushClient。
class SignalingClient : public QObject {
    Q_OBJECT
public:
    // 当前类只管理信令连接；WebRTCPushClient 在 request 到来时按会话动态创建。
    explicit SignalingClient(QObject* parent = nullptr);
    ~SignalingClient() = default;

    // 主动连接信令服务器。
    void connectToServer(const QString& url);

private slots:
    // WebSocket 生命周期回调。
    void onConnected();
    void onTextMessageReceived(const QString& message);
    void onDisconnected();

private:
    // 把 WebRTCPushClient 产生的本地 SDP/ICE 回调重新封装成 JSON 发给信令服务器。
    void setupCallbacks(std::shared_ptr<WebRTCPushClient> rtcClient);
    
    // 统一的 JSON 发送出口，避免各处重复构造文本消息。
    void sendJson(const QJsonObject& json);

    // 与信令服务器的唯一长连接。
    QWebSocket m_webSocket;

    // 一个接收端请求对应一个推流实例，key 为会话 id。
    std::unordered_map<std::string, std::shared_ptr<WebRTCPushClient>> clients{};
};