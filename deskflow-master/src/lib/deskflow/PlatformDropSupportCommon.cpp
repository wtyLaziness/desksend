/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */

#include "deskflow/PlatformDropSupport.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QDir>
#include <QDragEnterEvent>
#include <QMimeData>
#include <QPainter>
#include <QPointer>
#include <QThread>
#if defined(DESKSEND_HAVE_X11)
#include <QScreen>
#endif
#include <QTimer>
#include <QWidget>

#include <array>
#include <atomic>
#include <mutex>
#include <utility>

namespace deskflow::platform_drop {
namespace {

QCoreApplication *guiApplication()
{
  auto *app = QCoreApplication::instance();
  return app && app->inherits("QApplication") && !QCoreApplication::closingDown() ? app : nullptr;
}

template <typename F> void runOnGuiThread(F &&fn)
{
  if (auto *app = guiApplication()) {
    if (QThread::currentThread() == app->thread()) {
      fn();
    } else {
      QMetaObject::invokeMethod(app, std::forward<F>(fn), Qt::QueuedConnection);
    }
  }
}

#if defined(DESKSEND_HAVE_X11)
QRect nativeScreenGeometry(const QScreen *screen)
{
  const auto logical = screen->geometry();
  return {logical.topLeft(), logical.size() * screen->devicePixelRatio()};
}

QRect edgeWidgetGeometry(const QRect &native)
{
  QRect best;
  const QScreen *selected = nullptr;
  for (const auto *screen : QGuiApplication::screens()) {
    const auto frame = nativeScreenGeometry(screen);
    const auto part = frame.intersected(native);
    if (part.isEmpty() || qint64(part.width()) * part.height() <= qint64(best.width()) * best.height()) {
      continue;
    }
    best = part;
    selected = screen;
  }
  if (!selected) {
    return {};
  }
  const auto origin = selected->geometry().topLeft();
  const auto scale = selected->devicePixelRatio();
  const auto topLeft = origin + (best.topLeft() - origin) / scale;
  const auto end = origin + (best.topLeft() + QPoint(best.width(), best.height()) - origin) / scale;
  return {topLeft, QSize(qMax(1, end.x() - topLeft.x()), qMax(1, end.y() - topLeft.y()))};
}
#endif

class EdgeWidget final : public QWidget
{
public:
  EdgeWidget(int sideIndex, EdgeSentinelCallbacks callbacks)
      : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint |
                             Qt::WindowDoesNotAcceptFocus),
        m_sideIndex(sideIndex),
        m_callbacks(std::move(callbacks))
  {
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TranslucentBackground);
    setAcceptDrops(true);
  }

  void setNativeGeometry(const QRect &native)
  {
#if defined(DESKSEND_HAVE_X11)
    setGeometry(edgeWidgetGeometry(native));
#else
    setGeometry(native);
#endif
  }

protected:
  void dragEnterEvent(QDragEnterEvent *event) override
  {
    auto files = extractLocalFilePathsFromUrls(event->mimeData()->urls());
    if (files.empty()) {
      event->ignore();
      return;
    }
    auto global = mapToGlobal(event->position().toPoint());
#if defined(DESKSEND_HAVE_X11)
    const auto *edgeScreen = screen();
    if (!edgeScreen) {
      event->ignore();
      return;
    }
    const auto origin = edgeScreen->geometry().topLeft();
    global = origin + (global - origin) * edgeScreen->devicePixelRatio();
#endif
    event->acceptProposedAction();
    m_callbacks.onDragEnter(m_sideIndex, std::move(files), global.x(), global.y());
  }

  void dragLeaveEvent(QDragLeaveEvent *) override { m_callbacks.onDragLeave(); }
  void dropEvent(QDropEvent *event) override { event->ignore(); }
  void paintEvent(QPaintEvent *) override
  {
    QPainter painter(this);
    // 保持极低非零透明度，确保 macOS 与合成桌面下的 X11 边缘窗口能够正常捕获拖拽事件。
    painter.fillRect(rect(), QColor(0, 0, 0, 1));
  }

private:
  int m_sideIndex;
  EdgeSentinelCallbacks m_callbacks;
};

} // namespace

QList<QUrl> buildLocalFileUrls(const std::vector<std::string> &files)
{
  QList<QUrl> urls;
  for (const auto &file : files) {
    if (!file.empty()) {
      urls.append(QUrl::fromLocalFile(QDir::fromNativeSeparators(QString::fromStdString(file))));
    }
  }
  return urls;
}

std::vector<std::string> extractLocalFilePathsFromUrls(const QList<QUrl> &urls)
{
  std::vector<std::string> files;
  for (const auto &url : urls) {
    if (url.isValid() && url.isLocalFile() && !url.toLocalFile().isEmpty()) {
      files.push_back(url.toLocalFile().toStdString());
    }
  }
  return files;
}

QByteArray buildGnomeCopiedFilesPayload(const std::vector<std::string> &files)
{
  QByteArray payload("copy");
  for (const auto &url : buildLocalFileUrls(files)) {
    payload += '\n';
    payload += url.toEncoded();
  }
  return payload;
}

std::string parseFinderPathOutput(const std::string &stdoutText)
{
  for (const auto &line : QString::fromStdString(stdoutText).split(u'\n')) {
    const auto path = line.trimmed();
    if (path.startsWith(u'/')) {
      return path.toStdString();
    }
  }
  return {};
}

void copyFilesToPlatformClipboard(const std::vector<std::string> &files)
{
  if (files.empty()) {
    return;
  }
  runOnGuiThread([files] {
    const auto urls = buildLocalFileUrls(files);
    auto *clipboard = QApplication::clipboard();
    if (!clipboard || urls.isEmpty()) {
      return;
    }
    auto *mimeData = new QMimeData;
    mimeData->setUrls(urls);
#if defined(Q_OS_LINUX)
    mimeData->setData("x-special/gnome-copied-files", buildGnomeCopiedFilesPayload(files));
#endif
    clipboard->setMimeData(mimeData);
  });
}

struct QtEdgeSentinelHost::Impl
{
  template <typename F> void dispatch(const std::shared_ptr<Impl> &state, F &&fn)
  {
    runOnGuiThread([weak = std::weak_ptr<Impl>(state), fn = std::forward<F>(fn)]() mutable {
      if (auto self = weak.lock(); self && !self->stopped) {
        fn(*self);
      }
    });
  }

  void closeWindows(bool clearCallbacks)
  {
    if (clearCallbacks) {
      std::scoped_lock lock(callbackMutex);
      callbacks = {};
    }
    ++generation;
    cancelling = false;
    lastX = 0;
    lastY = 0;
    lastW = 0;
    lastH = 0;
    for (auto &edge : edges) {
      if (edge) {
        edge->hide();
        edge->deleteLater();
        edge.clear();
      }
    }
  }

  std::atomic_bool stopped = false;
  mutable std::mutex callbackMutex;
  EdgeSentinelCallbacks callbacks;
  std::array<QPointer<EdgeWidget>, 4> edges;
  bool aboutToQuitConnected = false;
  bool cancelling = false;
  uint64_t generation = 0;
  int32_t lastX = 0;
  int32_t lastY = 0;
  int32_t lastW = 0;
  int32_t lastH = 0;
};

QtEdgeSentinelHost::QtEdgeSentinelHost() : m_impl(std::make_shared<Impl>()) {}
QtEdgeSentinelHost::~QtEdgeSentinelHost()
{
  if (!m_impl->stopped.exchange(true)) {
    runOnGuiThread([state = m_impl] { state->closeWindows(true); });
  }
}

void QtEdgeSentinelHost::setCallbacks(EdgeSentinelCallbacks callbacks)
{
  {
    std::scoped_lock lock(m_impl->callbackMutex);
    m_impl->callbacks = std::move(callbacks);
  }
}

void QtEdgeSentinelHost::ensureInitialized(int32_t x, int32_t y, int32_t w, int32_t h)
{
  m_impl->dispatch(m_impl, [weak = std::weak_ptr<Impl>(m_impl), x, y, w, h](Impl &state) {
    if (!isNativeEdgeSentinelSupported() || w <= 0 || h <= 0) {
      return;
    }
    const std::array<QRect, 4> bounds{
        QRect(x, y, 2, h), QRect(x + (w - 2), y, 2, h), QRect(x, y, w, 2), QRect(x, y + (h - 2), w, 2)
    };
    if (state.edges.front()) {
      if (state.lastX != x || state.lastY != y || state.lastW != w || state.lastH != h) {
        state.lastX = x;
        state.lastY = y;
        state.lastW = w;
        state.lastH = h;
        for (std::size_t idx = 0; idx < bounds.size(); ++idx) {
          if (state.edges[idx]) {
            state.edges[idx]->setNativeGeometry(bounds[idx]);
          }
        }
      }
      return;
    }
    if (!state.aboutToQuitConnected) {
      state.aboutToQuitConnected = true;
      auto *app = QCoreApplication::instance();
      QObject::connect(app, &QCoreApplication::aboutToQuit, app, [weak] {
        if (auto self = weak.lock()) {
          self->stopped = true;
          self->closeWindows(true);
        }
      });
    }
    state.lastX = x;
    state.lastY = y;
    state.lastW = w;
    state.lastH = h;
    for (std::size_t idx = 0; idx < bounds.size(); ++idx) {
      EdgeSentinelCallbacks callbacks;
      callbacks.onDragEnter = [weak](int sideIndex, std::vector<std::string> files, int32_t gx, int32_t gy) {
        if (auto self = weak.lock(); self && !self->stopped) {
          std::function<void(int, std::vector<std::string>, int32_t, int32_t)> notify;
          {
            std::scoped_lock lock(self->callbackMutex);
            notify = self->callbacks.onDragEnter;
          }
          if (notify) {
            notify(sideIndex, std::move(files), gx, gy);
          }
        }
      };
      callbacks.onDragLeave = [weak] {
        if (auto self = weak.lock(); self && !self->stopped) {
          std::function<void()> notify;
          {
            std::scoped_lock lock(self->callbackMutex);
            notify = self->callbacks.onDragLeave;
          }
          if (notify) {
            notify();
          }
        }
      };
      auto *edge = new EdgeWidget(static_cast<int>(idx), std::move(callbacks));
      state.edges[idx] = edge;
      edge->setNativeGeometry(bounds[idx]);
      configurePlatformOverlayWidget(edge, false);
    }
  });
}

void QtEdgeSentinelHost::setEdgeGeometryAndVisible(
    std::size_t idx, int32_t rx, int32_t ry, int32_t rw, int32_t rh, bool show
)
{
  m_impl->dispatch(m_impl, [=](Impl &state) {
    if (idx < state.edges.size() && state.edges[idx]) {
      auto *edge = state.edges[idx].data();
      if (rw > 0 && rh > 0) {
        edge->setNativeGeometry(QRect(rx, ry, rw, rh));
        edge->setVisible(show);
      } else {
        edge->hide();
      }
    }
  });
}

void QtEdgeSentinelHost::setEdgeVisible(std::size_t idx, bool show)
{
  m_impl->dispatch(m_impl, [idx, show](Impl &state) {
    if (idx < state.edges.size() && state.edges[idx]) {
      state.edges[idx]->setVisible(show);
    }
  });
}

void QtEdgeSentinelHost::hideAll()
{
  m_impl->dispatch(m_impl, [](Impl &state) {
    for (auto &edge : state.edges) {
      if (edge) {
        edge->hide();
      }
    }
  });
}

void QtEdgeSentinelHost::clearCancellationFlags()
{
  m_impl->dispatch(m_impl, [](Impl &state) {
    state.cancelling = false;
    ++state.generation;
  });
}

void QtEdgeSentinelHost::cancelBeforeSwitch(int sideIndex)
{
  auto notifyImmediate = [impl = m_impl](bool stillPressed) {
    std::function<void(bool)> notify;
    {
      std::scoped_lock lock(impl->callbackMutex);
      notify = impl->callbacks.onCancelComplete;
    }
    if (notify) {
      notify(stillPressed);
    }
  };
  if (m_impl->stopped || !guiApplication() || !isNativeEdgeSentinelSupported()) {
    notifyImmediate(false);
    return;
  }
  m_impl->dispatch(m_impl, [weak = std::weak_ptr<Impl>(m_impl), sideIndex, notifyImmediate](Impl &state) {
    if (state.cancelling) {
      return;
    }
    if (sideIndex < 0 || static_cast<std::size_t>(sideIndex) >= state.edges.size() || !state.edges[sideIndex]) {
      notifyImmediate(false);
      return;
    }
    state.cancelling = true;
    const auto generation = ++state.generation;
    postNativeEscapeKey();
    QTimer::singleShot(80, state.edges[sideIndex].data(), [weak, generation, notifyImmediate] {
      if (auto self = weak.lock(); self && !self->stopped && self->generation == generation) {
        self->cancelling = false;
        notifyImmediate(isNativeLeftMouseButtonDown(false));
      }
    });
  });
}

void QtEdgeSentinelHost::shutdown()
{
  if (!m_impl->stopped) {
    m_impl->dispatch(m_impl, [](Impl &state) { state.closeWindows(false); });
  }
}

#if defined(_WIN32)
bool isNativeEdgeSentinelSupported() { return false; }
bool isNativeLeftMouseButtonDown(bool trackedFallback) { return trackedFallback; }
void postNativeEscapeKey() {}
void postNativeRelativeMouseMove(int, int) {}
void postNativeLeftMouseUp() {}
std::string resolveNonWindowsDropTargetDirectory(int32_t, int32_t, const std::string &fallbackDir, bool *direct)
{
  if (direct) {
    *direct = false;
  }
  return fallbackDir;
}
void configurePlatformOverlayWidget(QWidget *, bool) {}
#endif

} // namespace deskflow::platform_drop
