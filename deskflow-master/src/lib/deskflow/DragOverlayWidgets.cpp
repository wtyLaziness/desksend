/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 DeskSend Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/DragOverlayWidgets.h"

#include "deskflow/PlatformDropSupport.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QLabel>
#include <QLocale>
#include <QMetaObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>
#include <mutex>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace deskflow {

namespace {

bool hasGuiApplication()
{
  auto *app = QCoreApplication::instance();
  return app != nullptr && app->inherits("QApplication");
}

template <typename Fn> void runOnGuiThread(Fn &&fn)
{
  auto *app = QCoreApplication::instance();
  if (!app || !app->inherits("QApplication")) {
    return;
  }
  if (QThread::currentThread() != app->thread()) {
    QMetaObject::invokeMethod(app, std::forward<Fn>(fn), Qt::QueuedConnection);
    return;
  }
  fn();
}

void applyWin32ClickThroughStyle(QWidget *widget)
{
#if defined(_WIN32)
  if (!widget) {
    return;
  }
  HWND hwnd = reinterpret_cast<HWND>(widget->winId());
  if (hwnd != nullptr) {
    LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    exStyle |= WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_LAYERED;
    SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle);
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  }
#else
  (void)widget;
#endif
}

class DragHoverWidget final : public QWidget
{
public:
  explicit DragHoverWidget(const std::function<void()> &onTimeout)
      : QWidget(
            nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput |
                         Qt::WindowDoesNotAcceptFocus
        )
  {
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_TranslucentBackground);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    m_label = new QLabel(this);
    m_label->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  background-color: rgba(24, 28, 36, 230);"
        "  color: #F5F7FA;"
        "  border: 1px solid rgba(255, 255, 255, 56);"
        "  border-radius: 12px;"
        "  padding: 6px 12px;"
        "  font-size: 12px;"
        "  font-weight: 600;"
        "}"
    ));
    layout->addWidget(m_label);

    m_autoHideTimer.setSingleShot(true);
    QObject::connect(&m_autoHideTimer, &QTimer::timeout, this, [this, onTimeout]() {
      hide();
      if (onTimeout) {
        onTimeout();
      }
    });
  }

  void showAt(uint32_t fileCount, int32_t x, int32_t y)
  {
    m_label->setText(formatDragHoverText(fileCount));
    adjustSize();
    moveToCursor(x, y);
    if (!isVisible()) {
      show();
      applyWin32ClickThroughStyle(this);
    }
    m_autoHideTimer.start(DragHoverIndicator::kAutoHideMs);
  }

  void moveToCursor(int32_t x, int32_t y)
  {
    const QPoint cursor(x, y);
    QScreen *screen = QGuiApplication::screenAt(cursor);
    if (!screen) {
      screen = QGuiApplication::primaryScreen();
    }
    const QRect bounds = screen ? screen->availableGeometry() : QRect();
    const QPoint targetPos = clampDragHoverPosition(cursor, size(), bounds);
    move(targetPos);
    if (isVisible()) {
      m_autoHideTimer.start(DragHoverIndicator::kAutoHideMs);
    }
  }

  void hideOverlay()
  {
    m_autoHideTimer.stop();
    hide();
  }

protected:
  void showEvent(QShowEvent *event) override
  {
    QWidget::showEvent(event);
#if defined(_WIN32)
    applyWin32ClickThroughStyle(this);
#else
    platform_drop::configurePlatformOverlayWidget(this, true);
#endif
  }

private:
  QLabel *m_label = nullptr;
  QTimer m_autoHideTimer;
};

// 所有状态窗口操作均在 Qt 主线程执行，不抢走拖拽和键鼠输入的焦点。
class TransferStatusWidget final : public QWidget
{
public:
  explicit TransferStatusWidget(bool receiving, const std::function<void()> &onHideTimeout)
      : QWidget(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint),
        m_receiving(receiving)
  {
    setWindowTitle(
        receiving ? QCoreApplication::translate("FileDropBridge", "DeskSend · Receiving")
                  : QCoreApplication::translate("FileDropBridge", "DeskSend · Sending")
    );
    setAttribute(Qt::WA_ShowWithoutActivating);
    setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    setMinimumWidth(380);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    m_title = new QLabel(this);
    m_title->setWordWrap(true);
    m_bar = new QProgressBar(this);
    m_bar->setAccessibleName(QCoreApplication::translate("FileDropBridge", "File transfer progress"));
    m_details = new QLabel(this);
    m_details->setWordWrap(true);
    layout->addWidget(m_title);
    layout->addWidget(m_bar);
    layout->addWidget(m_details);
    m_cancelButton = new QPushButton(QCoreApplication::translate("FileDropBridge", "Cancel transfer"), this);
    m_cancelButton->setFocusPolicy(Qt::NoFocus);
    m_cancelButton->hide();
    layout->addWidget(m_cancelButton, 0, Qt::AlignRight);
    QObject::connect(m_cancelButton, &QPushButton::clicked, this, [this]() {
      auto cancel = std::move(m_cancel);
      if (cancel) {
        m_cancelButton->setEnabled(false);
        m_details->setText(QCoreApplication::translate("FileDropBridge", "Cancelling transfer..."));
        cancel();
      }
    });
    m_hide.setSingleShot(true);
    QObject::connect(&m_hide, &QTimer::timeout, this, [this, onHideTimeout]() {
      hide();
      if (onHideTimeout) {
        onHideTimeout();
      }
    });
  }

  void beginPreparing(uint32_t fileCount)
  {
    m_hide.stop();
    m_cancel = {};
    m_done = 0;
    m_total = 0;
    m_title->setText(
        QCoreApplication::translate("FileDropBridge", "Receiving %1 file(s)").arg(static_cast<qulonglong>(fileCount))
    );
    m_bar->setRange(0, 0);
    m_details->setText(QCoreApplication::translate("FileDropBridge", "Preparing transfer..."));
    m_cancelButton->hide();
    positionAndShow();
    // 防止在本地连接建立前远端断开导致准备提示永久残留。
    m_hide.start(15000);
  }

  void reset()
  {
    m_cancel = {};
    m_hide.stop();
    hide();
  }

  void begin(const QString &description, std::function<void()> cancel)
  {
    m_hide.stop();
    m_elapsed.start();
    m_done = 0;
    m_total = 0;
    m_title->setText(description);
    m_bar->setRange(0, 0);
    m_details->setText(QCoreApplication::translate("FileDropBridge", "Preparing transfer..."));
    m_cancel = std::move(cancel);
    m_cancelButton->setEnabled(true);
    m_cancelButton->setVisible(static_cast<bool>(m_cancel));
    positionAndShow();
  }

  void progress(quint64 done, quint64 total)
  {
    m_done = qMin(done, total);
    m_total = total;
    m_bar->setRange(0, 100);
    m_bar->setValue(total ? static_cast<int>(100.0 * m_done / total) : 0);
    if (!m_cancelButton->isEnabled()) {
      return;
    }
    const double seconds = qMax<qint64>(1, m_elapsed.elapsed()) / 1000.0;
    const double speed = m_done / seconds;
    const QString eta = speed > 0
                            ? QCoreApplication::translate("FileDropBridge", "%1 s").arg(
                                  static_cast<quint64>((total - m_done) / speed)
                              )
                            : QCoreApplication::translate("FileDropBridge", "estimating");
    const QLocale locale;
    m_details->setText(
        QCoreApplication::translate("FileDropBridge", "%1 / %2\n%3/s · Elapsed %4 s · Remaining %5")
            .arg(
                locale.formattedDataSize(m_done), locale.formattedDataSize(total),
                locale.formattedDataSize(static_cast<qint64>(speed))
            )
            .arg(static_cast<quint64>(seconds))
            .arg(eta)
    );
  }

  void finish(bool success, const QString &message, bool cancelled)
  {
    m_cancel = {};
    m_cancelButton->setEnabled(true);
    m_cancelButton->hide();
    if (success) {
      progress(m_total, m_total);
      m_bar->setValue(100);
    } else {
      m_bar->setRange(0, 100);
    }
    m_title->setText(message);
    if (cancelled) {
      m_details->clear();
    } else if (!success) {
      m_details->setText(
          QCoreApplication::translate(
              "FileDropBridge", "Check network connection, receiver port, and localsend-cli.exe; see logs for details."
          )
      );
    }
    show();
    m_hide.start(success || cancelled ? 8000 : 20000);
  }

#if !defined(_WIN32)
protected:
  void showEvent(QShowEvent *event) override
  {
    QWidget::showEvent(event);
    platform_drop::configurePlatformOverlayWidget(this, false);
  }
#endif

private:
  void positionAndShow()
  {
    adjustSize();
    if (auto *screen = QGuiApplication::primaryScreen()) {
      const auto area = screen->availableGeometry();
      move(area.right() - width() - 20, area.bottom() - height() - (m_receiving ? 200 : 20));
    }
    show();
  }

  bool m_receiving;
  QLabel *m_title = nullptr;
  QLabel *m_details = nullptr;
  QProgressBar *m_bar = nullptr;
  QPushButton *m_cancelButton = nullptr;
  std::function<void()> m_cancel;
  QElapsedTimer m_elapsed;
  QTimer m_hide;
  quint64 m_done = 0;
  quint64 m_total = 0;
};

} // namespace

QString formatDragHoverText(uint32_t fileCount)
{
  return QCoreApplication::translate("DragHoverIndicator", "Drop to receive %1 file(s)")
      .arg(static_cast<qulonglong>(fileCount));
}

QPoint clampDragHoverPosition(
    const QPoint &cursorPos, const QSize &widgetSize, const QRect &screenBounds, const QPoint &offset
)
{
  int x = cursorPos.x() + offset.x();
  int y = cursorPos.y() + offset.y();
  if (!screenBounds.isEmpty()) {
    const int w = std::max(0, widgetSize.width());
    const int h = std::max(0, widgetSize.height());
    const int maxX = std::max(screenBounds.left(), screenBounds.left() + screenBounds.width() - w);
    const int maxY = std::max(screenBounds.top(), screenBounds.top() + screenBounds.height() - h);
    x = std::clamp(x, screenBounds.left(), maxX);
    y = std::clamp(y, screenBounds.top(), maxY);
  }
  return QPoint(x, y);
}

struct DragHoverIndicator::Impl
{
  explicit Impl(bool stateOnlyMode) : stateOnly(stateOnlyMode) {}

  const bool stateOnly;
  mutable std::mutex mutex;
  bool visible = false;
  uint32_t fileCount = 0;
  QPoint cursorPos;
  QPointer<DragHoverWidget> widget;

  ~Impl()
  {
    if (widget && QCoreApplication::instance()) {
      widget->deleteLater();
    }
  }
};

DragHoverIndicator::DragHoverIndicator(bool stateOnly) : m_impl(std::make_shared<Impl>(stateOnly))
{
}

DragHoverIndicator::~DragHoverIndicator() = default;

void DragHoverIndicator::showHover(uint32_t fileCount, int32_t x, int32_t y)
{
  {
    std::scoped_lock lock(m_impl->mutex);
    m_impl->visible = true;
    m_impl->fileCount = fileCount;
    m_impl->cursorPos = QPoint(x, y);
  }
  if (m_impl->stateOnly || !hasGuiApplication()) {
    return;
  }
  std::weak_ptr<Impl> weak = m_impl;
  runOnGuiThread([weak, fileCount, x, y]() {
    auto impl = weak.lock();
    if (!impl) {
      return;
    }
    if (!impl->widget) {
      impl->widget = new DragHoverWidget([weak]() {
        if (auto inner = weak.lock()) {
          std::scoped_lock lock(inner->mutex);
          inner->visible = false;
        }
      });
    }
    impl->widget->showAt(fileCount, x, y);
  });
}

void DragHoverIndicator::updateCursor(int32_t x, int32_t y)
{
  {
    std::scoped_lock lock(m_impl->mutex);
    if (!m_impl->visible) {
      return;
    }
    m_impl->cursorPos = QPoint(x, y);
  }
  if (m_impl->stateOnly || !hasGuiApplication()) {
    return;
  }
  std::weak_ptr<Impl> weak = m_impl;
  runOnGuiThread([weak, x, y]() {
    auto impl = weak.lock();
    if (!impl || !impl->widget) {
      return;
    }
    impl->widget->moveToCursor(x, y);
  });
}

void DragHoverIndicator::hideHover()
{
  {
    std::scoped_lock lock(m_impl->mutex);
    m_impl->visible = false;
  }
  if (m_impl->stateOnly || !hasGuiApplication()) {
    return;
  }
  std::weak_ptr<Impl> weak = m_impl;
  runOnGuiThread([weak]() {
    auto impl = weak.lock();
    if (!impl || !impl->widget) {
      return;
    }
    impl->widget->hideOverlay();
  });
}

bool DragHoverIndicator::isVisible() const
{
  std::scoped_lock lock(m_impl->mutex);
  return m_impl->visible;
}

uint32_t DragHoverIndicator::fileCount() const
{
  std::scoped_lock lock(m_impl->mutex);
  return m_impl->fileCount;
}

QPoint DragHoverIndicator::cursorPos() const
{
  std::scoped_lock lock(m_impl->mutex);
  return m_impl->cursorPos;
}

struct TransferStatusOverlay::Impl
{
  explicit Impl(bool isReceiving, bool stateOnlyMode) : receiving(isReceiving), stateOnly(stateOnlyMode) {}

  ~Impl()
  {
    if (widget && QCoreApplication::instance()) {
      widget->deleteLater();
    }
  }

  TransferStatusWidget *ensureWidgetLocked(const std::weak_ptr<Impl> &weak)
  {
    if (!widget) {
      widget = new TransferStatusWidget(receiving, [weak]() {
        if (auto inner = weak.lock()) {
          std::scoped_lock lock(inner->mutex);
          if (inner->phase == Phase::Preparing || inner->phase == Phase::Finished) {
            inner->phase = Phase::Idle;
            inner->preparingFileCount = 0;
          }
        }
      });
    }
    return widget;
  }

  const bool receiving;
  const bool stateOnly;
  mutable std::mutex mutex;
  Phase phase = Phase::Idle;
  uint32_t preparingFileCount = 0;
  QPointer<TransferStatusWidget> widget;
};

TransferStatusOverlay::TransferStatusOverlay(bool receiving, bool stateOnly)
    : m_impl(std::make_shared<Impl>(receiving, stateOnly))
{
}

TransferStatusOverlay::~TransferStatusOverlay() = default;

void TransferStatusOverlay::beginPreparing(uint32_t fileCount)
{
  {
    std::scoped_lock lock(m_impl->mutex);
    if (m_impl->phase == Phase::Active) {
      return;
    }
    m_impl->phase = Phase::Preparing;
    m_impl->preparingFileCount = fileCount;
  }
  if (m_impl->stateOnly || !hasGuiApplication()) {
    return;
  }
  std::weak_ptr<Impl> weak = m_impl;
  runOnGuiThread([weak, fileCount]() {
    if (auto impl = weak.lock()) {
      impl->ensureWidgetLocked(weak)->beginPreparing(fileCount);
    }
  });
}

void TransferStatusOverlay::resetOverlayPhase(bool onlyIfPreparing)
{
  {
    std::scoped_lock lock(m_impl->mutex);
    if (onlyIfPreparing && m_impl->phase != Phase::Preparing) {
      return;
    }
    m_impl->phase = Phase::Idle;
    m_impl->preparingFileCount = 0;
  }
  if (m_impl->stateOnly || !hasGuiApplication()) {
    return;
  }
  std::weak_ptr<Impl> weak = m_impl;
  runOnGuiThread([weak]() {
    if (auto impl = weak.lock(); impl && impl->widget) {
      std::scoped_lock lock(impl->mutex);
      if (impl->phase == Phase::Idle) {
        impl->widget->reset();
      }
    }
  });
}

void TransferStatusOverlay::cancelPreparing()
{
  resetOverlayPhase(true);
}

void TransferStatusOverlay::resetToIdle()
{
  resetOverlayPhase(false);
}

void TransferStatusOverlay::begin(const QString &description, std::function<void()> cancel)
{
  {
    std::scoped_lock lock(m_impl->mutex);
    m_impl->phase = Phase::Active;
    m_impl->preparingFileCount = 0;
  }
  if (m_impl->stateOnly || !hasGuiApplication()) {
    return;
  }
  std::weak_ptr<Impl> weak = m_impl;
  runOnGuiThread([weak, description, cancel = std::move(cancel)]() mutable {
    if (auto impl = weak.lock()) {
      impl->ensureWidgetLocked(weak)->begin(description, std::move(cancel));
    }
  });
}

void TransferStatusOverlay::progress(uint64_t done, uint64_t total)
{
  if (m_impl->stateOnly || !hasGuiApplication()) {
    return;
  }
  std::weak_ptr<Impl> weak = m_impl;
  runOnGuiThread([weak, done, total]() {
    if (auto impl = weak.lock()) {
      impl->ensureWidgetLocked(weak)->progress(done, total);
    }
  });
}

void TransferStatusOverlay::finish(bool success, const QString &message)
{
  finish(success, message, false);
}

void TransferStatusOverlay::finishCancelled()
{
  finish(false, QCoreApplication::translate("FileDropBridge", "File transfer cancelled"), true);
}

void TransferStatusOverlay::finish(bool success, const QString &message, bool cancelled)
{
  {
    std::scoped_lock lock(m_impl->mutex);
    m_impl->phase = Phase::Finished;
    m_impl->preparingFileCount = 0;
  }
  if (m_impl->stateOnly || !hasGuiApplication()) {
    return;
  }
  std::weak_ptr<Impl> weak = m_impl;
  runOnGuiThread([weak, success, message, cancelled]() {
    if (auto impl = weak.lock()) {
      impl->ensureWidgetLocked(weak)->finish(success, message, cancelled);
    }
  });
}

TransferStatusOverlay::Phase TransferStatusOverlay::phase() const
{
  std::scoped_lock lock(m_impl->mutex);
  return m_impl->phase;
}

uint32_t TransferStatusOverlay::preparingFileCount() const
{
  std::scoped_lock lock(m_impl->mutex);
  return m_impl->preparingFileCount;
}

} // namespace deskflow
