#include "WebsocketClient.h"

#include <QDebug>

// 构造时立即建立连接，并把底层状态事件统一转成当前类对外暴露的信号。
WebsocketClient::WebsocketClient(const QUrl &url, QObject *parent)
: QObject(parent), m_url(url)
{
    connect(&m_webSocket, &QWebSocket::connected, this, &WebsocketClient::onConnected);
    connect(&m_webSocket, &QWebSocket::disconnected, this, &WebsocketClient::onDisconnected);
    connect(&m_webSocket, &QWebSocket::textMessageReceived, this, &WebsocketClient::onTextMessageReceived);

    m_webSocket.open(m_url);
}

void WebsocketClient::sendMessage(const QString &message)
{
    // 当前封装只处理文本协议，适合 JSON 信令这类轻量消息。
    m_webSocket.sendTextMessage(message);
}

void WebsocketClient::onConnected()
{
    qDebug() << "WebSocket connected to" << m_url.toString();
    emit connected();
}

void WebsocketClient::onDisconnected()
{
    qDebug() << "WebSocket disconnected";
    emit disconnected();
}

void WebsocketClient::onTextMessageReceived(const QString &message)
{
    // 保留原始文本，解析职责交给更高一层的信令模块处理。
    qDebug() << "Message received:" << message;
    emit messageReceived(message);
}