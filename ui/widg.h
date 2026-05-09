#ifndef WIDG_H
#define WIDG_H

#include <QWidget>

#include "module/signaling_client.h"

namespace Ui
{
    class widg;
}

// 发送端控制窗口：只负责展示一个最小 UI，并触发 SignalingClient 进入待命状态。
class widg : public QWidget
{
    Q_OBJECT

public:
    explicit widg(bool autostart = false, QWidget *parent = nullptr);
    ~widg();

    // 连接信令服务器并把按钮切换为“等待 request”状态。
    void startSignaling();

private slots:
    // UI 按钮点击入口。
    void on_pushButton_clicked();

private:
    Ui::widg *ui;

    // 窗口持有唯一的 sender 侧信令客户端。
    SignalingClient* m_ptrSignalingClient{nullptr};

    // 避免重复点击导致多次连接同一信令地址。
    bool signaling_started_{false};
};

#endif // WIDG_H
