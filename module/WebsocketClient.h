#pragma once

#include <QObject>
#include <QWebSocket>

// 轻量级 WebSocket 包装层：
// 对外只暴露连接状态和文本消息信号，便于后续模块复用 Qt WebSocket 能力。
class WebsocketClient : public QObject
{
    Q_OBJECT
public:
    explicit WebsocketClient(const QUrl &url, QObject *parent = nullptr);

    // 向当前连接发送一条 UTF-8 文本消息。
    void sendMessage(const QString &message);

signals:
    // 套接字与服务端建立连接后发出。
    void connected();

    // 套接字断开后发出。
    void disconnected();

    // 收到服务端文本消息后转发给上层。
    void messageReceived(const QString &message);

private slots:
    // 将 QWebSocket 的底层信号翻译为当前类的统一事件接口。
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);

private:
    // 真正承载网络通信的 Qt WebSocket 实例。
    QWebSocket m_webSocket;

    // 构造时传入的目标地址，主要用于建立连接和日志输出。
    QUrl m_url;
};