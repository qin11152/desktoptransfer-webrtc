#pragma once

#include <QObject>
#include <QWebSocket>

class WebsocketClient : public QObject
{
    Q_OBJECT
public:
    explicit WebsocketClient(const QUrl &url, QObject *parent = nullptr);

    void sendMessage(const QString &message);

signals:
    void connected();
    void disconnected();
    void messageReceived(const QString &message);

private slots:
    void onConnected();
    void onDisconnected();
    void onTextMessageReceived(const QString &message);

private:
    QWebSocket m_webSocket;
    QUrl m_url;
};