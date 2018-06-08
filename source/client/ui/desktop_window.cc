//
// PROJECT:         Aspia
// FILE:            client/ui/desktop_window.cc
// LICENSE:         GNU General Public License 3
// PROGRAMMERS:     Dmitry Chapyshev (dmitry@aspia.ru)
//

#include "client/ui/desktop_window.h"

#include <QDebug>
#include <QApplication>
#include <QBrush>
#include <QDesktopWidget>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPalette>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>

#include "base/clipboard.h"
#include "client/ui/desktop_config_dialog.h"
#include "client/ui/desktop_panel.h"
#include "client/ui/desktop_widget.h"
#include "desktop_capture/desktop_frame_qimage.h"

namespace aspia {

DesktopWindow::DesktopWindow(proto::address_book::Computer* computer, QWidget* parent)
    : QWidget(parent),
      computer_(computer)
{
    QString session_name;
    if (computer_->session_type() == proto::auth::SESSION_TYPE_DESKTOP_MANAGE)
    {
        session_name = tr("Aspia Desktop Manage");
    }
    else
    {
        Q_ASSERT(computer_->session_type() == proto::auth::SESSION_TYPE_DESKTOP_VIEW);
        session_name = tr("Aspia Desktop View");
    }

    QString computer_name;
    if (!computer_->name().empty())
        computer_name = QString::fromStdString(computer_->name());
    else
        computer_name = QString::fromStdString(computer_->address());

    setWindowTitle(QString("%1 - %2").arg(computer_name).arg(session_name));
    setMinimumSize(800, 600);

    desktop_ = new DesktopWidget(this);

    scroll_area_ = new QScrollArea(this);
    scroll_area_->setAlignment(Qt::AlignHCenter | Qt::AlignVCenter);
    scroll_area_->setFrameShape(QFrame::NoFrame);
    scroll_area_->setAutoFillBackground(true);
    scroll_area_->setWidget(desktop_);

    QPalette palette(scroll_area_->palette());
    palette.setBrush(QPalette::Background, QBrush(QColor(25, 25, 25)));
    scroll_area_->setPalette(palette);

    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->addWidget(scroll_area_);

    panel_ = new DesktopPanel(computer_->session_type(), this);
    panel_->adjustSize();

    connect(panel_, &DesktopPanel::keySequence, desktop_, &DesktopWidget::executeKeySequense);
    connect(panel_, &DesktopPanel::settingsButton, this, &DesktopWindow::changeSettings);
    connect(panel_, &DesktopPanel::switchToAutosize, this, &DesktopWindow::autosizeWindow);

    connect(panel_, &DesktopPanel::switchToFullscreen, this, [this](bool fullscreen)
    {
        if (fullscreen)
        {
            is_maximized_ = isMaximized();
            showFullScreen();
        }
        else
        {
            if (is_maximized_)
                showMaximized();
            else
                showNormal();
        }
    });

    connect(desktop_, &DesktopWidget::sendPointerEvent, this, &DesktopWindow::onPointerEvent);
    connect(desktop_, &DesktopWidget::sendKeyEvent, this, &DesktopWindow::sendKeyEvent);
    connect(desktop_, &DesktopWidget::updated, panel_, QOverload<>::of(&DesktopPanel::update));

    desktop_->installEventFilter(this);
    scroll_area_->viewport()->installEventFilter(this);
}

void DesktopWindow::resizeDesktopFrame(const QSize& screen_size)
{
    QSize prev_size = desktop_->size();

    desktop_->resizeDesktopFrame(screen_size);

    if (screen_size != prev_size && !isMaximized() && !isFullScreen())
        autosizeWindow();
}

void DesktopWindow::drawDesktopFrame()
{
    desktop_->drawDesktopFrame();
}

DesktopFrame* DesktopWindow::desktopFrame()
{
    return desktop_->desktopFrame();
}

void DesktopWindow::injectCursor(const QCursor& cursor)
{
    desktop_->setCursor(cursor);
}

void DesktopWindow::injectClipboard(const proto::desktop::ClipboardEvent& event)
{
    if (!clipboard_.isNull())
        clipboard_->injectClipboardEvent(event);
}

void DesktopWindow::setSupportedVideoEncodings(quint32 video_encodings)
{
    supported_video_encodings_ = video_encodings;
}

void DesktopWindow::setSupportedFeatures(quint32 features)
{
    supported_features_ = features;

    if (computer_->session_type() == proto::auth::SESSION_TYPE_DESKTOP_MANAGE)
    {
        delete clipboard_;

        // If the clipboard is supported by the host.
        if (supported_features_ & proto::desktop::FEATURE_CLIPBOARD)
        {
            const proto::desktop::Config& config = computer_->session_config().desktop_manage();

            // If the clipboard is enabled in the config.
            if (config.features() & proto::desktop::FEATURE_CLIPBOARD)
            {
                clipboard_ = new Clipboard(this);

                connect(clipboard_, &Clipboard::clipboardEvent,
                        this, &DesktopWindow::sendClipboardEvent);
            }
        }
    }
    else
    {
        Q_ASSERT(computer_->session_type() == proto::auth::SESSION_TYPE_DESKTOP_VIEW);
    }
}

bool DesktopWindow::requireConfigChange(proto::desktop::Config* config)
{
    if (!(supported_video_encodings_ & config->video_encoding()))
    {
        QMessageBox::warning(this,
                             tr("Warning"),
                             tr("The current video encoding is not supported by the host. "
                                "Please specify a different video encoding."),
                             QMessageBox::Ok);
    }

    DesktopConfigDialog dialog(config, supported_video_encodings_, supported_features_, this);
    if (dialog.exec() != DesktopConfigDialog::Accepted)
        return false;

    setSupportedFeatures(supported_features_);
    return true;
}

void DesktopWindow::onPointerEvent(const QPoint& pos, quint32 mask)
{
    QPoint cursor = desktop_->mapTo(scroll_area_, pos);
    QRect client_area = scroll_area_->rect();

    QScrollBar* hscrollbar = scroll_area_->horizontalScrollBar();
    QScrollBar* vscrollbar = scroll_area_->verticalScrollBar();

    if (!hscrollbar->isHidden())
        client_area.setHeight(client_area.height() - hscrollbar->height());

    if (!vscrollbar->isHidden())
        client_area.setWidth(client_area.width() - vscrollbar->width());

    scroll_delta_.setX(0);
    scroll_delta_.setY(0);

    if (client_area.width() < desktop_->width())
    {
        if (cursor.x() > client_area.width() - 50)
            scroll_delta_.setX(10);
        else if (cursor.x() < 50)
            scroll_delta_.setX(-10);
    }

    if (client_area.height() < desktop_->height())
    {
        if (cursor.y() > client_area.height() - 50)
            scroll_delta_.setY(10);
        else if (cursor.y() < 50)
            scroll_delta_.setY(-10);
    }

    if (!scroll_delta_.isNull())
    {
        if (scroll_timer_id_ == 0)
            scroll_timer_id_ = startTimer(15);
    }
    else if (scroll_timer_id_ != 0)
    {
        killTimer(scroll_timer_id_);
        scroll_timer_id_ = 0;
    }

    emit sendPointerEvent(pos, mask);
}

void DesktopWindow::changeSettings()
{
    proto::desktop::Config* config =
        computer_->mutable_session_config()->mutable_desktop_view();

    if (computer_->session_type() == proto::auth::SESSION_TYPE_DESKTOP_MANAGE)
        config = computer_->mutable_session_config()->mutable_desktop_manage();

    DesktopConfigDialog dialog(config, supported_video_encodings_, supported_features_, this);
    if (dialog.exec() == DesktopConfigDialog::Accepted)
    {
        setSupportedFeatures(supported_features_);
        emit sendConfig(*config);
    }
}

void DesktopWindow::autosizeWindow()
{
    QRect screen_rect = QApplication::desktop()->availableGeometry(this);
    QSize window_size = desktop_->size() + frameSize() - size();

    if (window_size.width() < screen_rect.width() && window_size.height() < screen_rect.height())
    {
        showNormal();

        resize(desktop_->size());
        move(screen_rect.x() + (screen_rect.width() / 2 - window_size.width() / 2),
             screen_rect.y() + (screen_rect.height() / 2 - window_size.height() / 2));
    }
    else
    {
        showMaximized();
    }
}

void DesktopWindow::timerEvent(QTimerEvent* event)
{
    if (event->timerId() == scroll_timer_id_)
    {
        if (scroll_delta_.x() != 0)
        {
            QScrollBar* scrollbar = scroll_area_->horizontalScrollBar();

            int pos = scrollbar->sliderPosition() + scroll_delta_.x();

            pos = qMax(pos, scrollbar->minimum());
            pos = qMin(pos, scrollbar->maximum());

            scrollbar->setSliderPosition(pos);
        }

        if (scroll_delta_.y() != 0)
        {
            QScrollBar* scrollbar = scroll_area_->verticalScrollBar();

            int pos = scrollbar->sliderPosition() + scroll_delta_.y();

            pos = qMax(pos, scrollbar->minimum());
            pos = qMin(pos, scrollbar->maximum());

            scrollbar->setSliderPosition(pos);
        }
    }

    QWidget::timerEvent(event);
}

void DesktopWindow::resizeEvent(QResizeEvent* event)
{
    panel_->move(QPoint(width() / 2 - panel_->width() / 2, 0));
    QWidget::resizeEvent(event);
}

void DesktopWindow::closeEvent(QCloseEvent* event)
{
    emit windowClose();
    QWidget::closeEvent(event);
}

bool DesktopWindow::eventFilter(QObject* object, QEvent* event)
{
    if (object == desktop_)
    {
        if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)
        {
            QKeyEvent* key_event = dynamic_cast<QKeyEvent*>(event);
            if (key_event && key_event->key() == Qt::Key_Tab)
            {
                desktop_->doKeyEvent(key_event);
                return true;
            }
        }

        return false;
    }
    else if (object == scroll_area_->viewport())
    {
        if (event->type() == QEvent::Wheel)
        {
            QWheelEvent* wheel_event = dynamic_cast<QWheelEvent*>(event);
            if (wheel_event)
            {
                QPoint pos = desktop_->mapFromGlobal(wheel_event->globalPos());

                desktop_->doMouseEvent(wheel_event->type(),
                                       wheel_event->buttons(),
                                       pos,
                                       wheel_event->angleDelta());
                return true;
            }
        }
    }

    return QWidget::eventFilter(object, event);
}

} // namespace aspia
