#ifndef WIDG_H
#define WIDG_H

#include <QWidget>

#include "module/signaling_client.h"

namespace Ui
{
    class widg;
}

class widg : public QWidget
{
    Q_OBJECT

public:
    explicit widg(bool autostart = false, QWidget *parent = nullptr);
    ~widg();

    void startSignaling();

private slots:
    void on_pushButton_clicked();

private:
    Ui::widg *ui;
    SignalingClient* m_ptrSignalingClient{nullptr};
    bool signaling_started_{false};
};

#endif // WIDG_H
