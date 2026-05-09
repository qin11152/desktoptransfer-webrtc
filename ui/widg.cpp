#include "widg.h"
#include "ui_widg.h"

#include "api/create_peerconnection_factory.h"
#include "api/environment/environment_factory.h"
#include "api/field_trials.h"
#include "rtc_base/thread.h"
#include "rtc_base/logging.h"
#include <QTimer>

widg::widg(bool autostart, QWidget *parent)
    : QWidget(parent), ui(new Ui::widg)
{
    ui->setupUi(this);
    setWindowTitle("WebRTC Screen Sender");
    ui->pushButton->setText("连接信令并等待推屏请求");
    m_ptrSignalingClient = new SignalingClient(this);

    if (autostart)
    {
        QTimer::singleShot(0, this, [this]() { startSignaling(); });
    }
}

widg::~widg()
{
    delete ui;
}

void widg::on_pushButton_clicked()
{
    startSignaling();
}

void widg::startSignaling()
{
    if (signaling_started_)
        return;

    signaling_started_ = true;
    ui->pushButton->setEnabled(false);
    ui->pushButton->setText("信令已连接，等待对端 request...");
    m_ptrSignalingClient->connectToServer("ws://localhost:8000/server");
}
