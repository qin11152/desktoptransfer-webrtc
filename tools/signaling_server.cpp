#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QPointer>
#include <QUrl>
#include <QWebSocket>
#include <QWebSocketServer>

struct SessionBinding
{
    QPointer<QWebSocket> sender;
    QPointer<QWebSocket> receiver;
};

class SignalingServer : public QObject
{
    Q_OBJECT

public:
    explicit SignalingServer(quint16 port, QObject *parent = nullptr)
        : QObject(parent), server_(QStringLiteral("webrtc-demo-signaling"), QWebSocketServer::NonSecureMode, this)
    {
        connect(&server_, &QWebSocketServer::newConnection, this, &SignalingServer::onNewConnection);

        if (!server_.listen(QHostAddress::Any, port))
        {
            qFatal("Failed to listen on port %d", port);
        }

        qInfo() << "Signaling server listening on ws://127.0.0.1:" << port << "/server";
    }

private:
    void onNewConnection()
    {
        auto *socket = server_.nextPendingConnection();
        if (!socket)
            return;

        sockets_.insert(socket);
        qInfo() << "Client connected" << socket;

        connect(socket, &QWebSocket::textMessageReceived, this, [this, socket](const QString &message)
                { onTextMessage(socket, message); });
        connect(socket, &QWebSocket::disconnected, this, [this, socket]()
                { onDisconnected(socket); });
    }

    void onTextMessage(QWebSocket *socket, const QString &message)
    {
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(message.toUtf8(), &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
        {
            qWarning() << "Invalid JSON from client:" << error.errorString() << message;
            return;
        }

        const QJsonObject json = document.object();
        const QString type = json.value(QStringLiteral("type")).toString();
        const QString id = json.value(QStringLiteral("id")).toString();

        if (type == QStringLiteral("sender-ready"))
        {
            ready_senders_.insert(socket);
            qInfo() << "Sender ready" << socket;
            return;
        }

        if (type == QStringLiteral("request"))
        {
            handleRequest(socket, json, id);
            return;
        }

        if (type == QStringLiteral("offer") || type == QStringLiteral("answer") || type == QStringLiteral("candidate") || type == QStringLiteral("error"))
        {
            forwardSessionMessage(socket, json, id);
            return;
        }

        qWarning() << "Unsupported message type:" << type << json;
    }

    void handleRequest(QWebSocket *receiver, const QJsonObject &json, const QString &id)
    {
        if (id.isEmpty())
        {
            sendError(receiver, QStringLiteral("request missing id"));
            return;
        }

        if (sessions_.contains(id))
        {
            sendError(receiver, QStringLiteral("session already exists: ") + id);
            return;
        }

        QWebSocket *selected_sender = nullptr;
        for (auto *candidate : ready_senders_)
        {
            if (candidate && candidate != receiver && !socket_to_session_.contains(candidate))
            {
                selected_sender = candidate;
                break;
            }
        }

        if (!selected_sender)
        {
            sendError(receiver, QStringLiteral("no sender-ready client available"));
            return;
        }

        SessionBinding binding;
        binding.sender = selected_sender;
        binding.receiver = receiver;
        sessions_.insert(id, binding);
        socket_to_session_.insert(selected_sender, id);
        socket_to_session_.insert(receiver, id);

        sendJson(selected_sender, json);
        qInfo() << "Forwarded request for session" << id;
    }

    void forwardSessionMessage(QWebSocket *socket, const QJsonObject &json, const QString &id)
    {
        if (id.isEmpty() || !sessions_.contains(id))
        {
            sendError(socket, QStringLiteral("unknown session id: ") + id);
            return;
        }

        const auto binding = sessions_.value(id);
        QWebSocket *target = nullptr;
        if (binding.sender == socket)
        {
            target = binding.receiver;
        }
        else if (binding.receiver == socket)
        {
            target = binding.sender;
        }

        if (!target)
        {
            sendError(socket, QStringLiteral("peer not connected for session: ") + id);
            return;
        }

        sendJson(target, json);
    }

    void onDisconnected(QWebSocket *socket)
    {
        qInfo() << "Client disconnected" << socket;
        ready_senders_.remove(socket);
        sockets_.remove(socket);

        const QString session_id = socket_to_session_.take(socket);
        if (!session_id.isEmpty())
        {
            const auto binding = sessions_.take(session_id);
            if (binding.sender)
            {
                socket_to_session_.remove(binding.sender);
                if (binding.sender != socket)
                    sendPeerDisconnected(binding.sender, session_id);
            }
            if (binding.receiver)
            {
                socket_to_session_.remove(binding.receiver);
                if (binding.receiver != socket)
                    sendPeerDisconnected(binding.receiver, session_id);
            }
        }

        socket->deleteLater();
    }

    void sendPeerDisconnected(QWebSocket *socket, const QString &id)
    {
        QJsonObject json;
        json[QStringLiteral("type")] = QStringLiteral("peer-disconnected");
        json[QStringLiteral("id")] = id;
        sendJson(socket, json);
    }

    void sendError(QWebSocket *socket, const QString &message)
    {
        QJsonObject json;
        json[QStringLiteral("type")] = QStringLiteral("error");
        json[QStringLiteral("message")] = message;
        sendJson(socket, json);
        qWarning() << message;
    }

    void sendJson(QWebSocket *socket, const QJsonObject &json)
    {
        if (!socket)
            return;

        socket->sendTextMessage(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
    }

private:
    QWebSocketServer server_;
    QSet<QWebSocket *> sockets_;
    QSet<QWebSocket *> ready_senders_;
    QMap<QString, SessionBinding> sessions_;
    QHash<QWebSocket *, QString> socket_to_session_;
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    SignalingServer server(8000);
    return app.exec();
}

#include "signaling_server.moc"