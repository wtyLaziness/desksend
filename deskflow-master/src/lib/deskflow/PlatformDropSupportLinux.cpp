/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */

#include "deskflow/PlatformDropSupport.h"

#include <QGuiApplication>
#include <QStandardPaths>
#include <QWidget>

#include <algorithm>
#include <memory>

#if defined(DESKSEND_HAVE_X11)
#include <unistd.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#if defined(DESKSEND_HAVE_XTEST)
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#endif
#undef None
#undef Bool
#undef Status
#undef CursorShape
#undef KeyPress
#undef KeyRelease
#endif

namespace deskflow::platform_drop {
namespace {

#if defined(DESKSEND_HAVE_X11)
using DisplayPtr = std::unique_ptr<Display, decltype(&XCloseDisplay)>;

DisplayPtr openXDisplay()
{
  return {isNativeEdgeSentinelSupported() ? XOpenDisplay(nullptr) : nullptr, XCloseDisplay};
}

std::vector<unsigned long> readProperty(Display *display, Window window, Atom property, Atom expectedType)
{
  Atom actualType = 0;
  int format = 0;
  unsigned long count = 0;
  unsigned long remaining = 0;
  unsigned char *data = nullptr;
  const auto status = XGetWindowProperty(
      display, window, property, 0, 1024, False, expectedType, &actualType, &format, &count, &remaining, &data
  );
  std::vector<unsigned long> values;
  if (status == Success && actualType == expectedType && format == 32 && data) {
    const auto *items = reinterpret_cast<unsigned long *>(data);
    values.assign(items, items + count);
  }
  if (data) {
    XFree(data);
  }
  return values;
}

std::vector<Window> childrenOf(Display *display, Window window)
{
  Window root = 0, parent = 0;
  Window *children = nullptr;
  unsigned count = 0;
  std::vector<Window> result;
  if (XQueryTree(display, window, &root, &parent, &children, &count) && children) {
    result.assign(children, children + count);
    XFree(children);
  }
  return result;
}

Window findClient(Display *display, Window window, Atom wmState)
{
  if (!readProperty(display, window, wmState, wmState).empty()) {
    return window;
  }
  const auto children = childrenOf(display, window);
  for (auto child = children.rbegin(); child != children.rend(); ++child) {
    if (auto client = findClient(display, *child, wmState)) {
      return client;
    }
  }
  return 0;
}

bool hitsDesktop(Display *display, Window root, int x, int y)
{
  const auto wmState = XInternAtom(display, "WM_STATE", False);
  const auto pidProperty = XInternAtom(display, "_NET_WM_PID", False);
  const auto typeProperty = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
  const auto desktopType = XInternAtom(display, "_NET_WM_WINDOW_TYPE_DESKTOP", False);

  // 暂停其他客户端请求，避免窗口在枚举和查询之间被销毁。
  XGrabServer(display);
  struct ServerGrab
  {
    Display *display;
    ~ServerGrab() { XUngrabServer(display); }
  } grab{display};

  const auto children = childrenOf(display, root);
  for (auto child = children.rbegin(); child != children.rend(); ++child) {
    XWindowAttributes attributes{};
    if (!XGetWindowAttributes(display, *child, &attributes) || attributes.map_state != IsViewable ||
        attributes.c_class == InputOnly) {
      continue;
    }
    int localX = 0, localY = 0;
    Window below = 0;
    if (!XTranslateCoordinates(display, root, *child, x, y, &localX, &localY, &below) ||
        localX < 0 || localY < 0 || localX >= attributes.width || localY >= attributes.height) {
      continue;
    }
    const auto managedClient = findClient(display, *child, wmState);
    const auto client = managedClient ? managedClient : *child;
    const auto pids = readProperty(display, client, pidProperty, XA_CARDINAL);
    if (!pids.empty() && pids.front() == static_cast<unsigned long>(getpid())) {
      continue;
    }
    const auto types = readProperty(display, client, typeProperty, XA_ATOM);
    return std::find(types.begin(), types.end(), desktopType) != types.end();
  }
  return true;
}

#if defined(DESKSEND_HAVE_XTEST)
template <typename F> void withXTest(F action)
{
  if (auto display = openXDisplay()) {
    int event = 0, error = 0, major = 0, minor = 0;
    if (XTestQueryExtension(display.get(), &event, &error, &major, &minor)) {
      action(display.get());
      XFlush(display.get());
    }
  }
}
#endif
#endif

} // namespace

bool isNativeEdgeSentinelSupported()
{
#if defined(DESKSEND_HAVE_X11)
  return QGuiApplication::platformName() == QStringLiteral("xcb");
#else
  return false;
#endif
}

bool isNativeLeftMouseButtonDown(bool trackedFallback)
{
#if defined(DESKSEND_HAVE_X11)
  if (auto display = openXDisplay()) {
    for (int screen = 0; screen < ScreenCount(display.get()); ++screen) {
      Window root = 0, child = 0;
      int rootX = 0, rootY = 0, x = 0, y = 0;
      unsigned mask = 0;
      if (XQueryPointer(display.get(), RootWindow(display.get(), screen), &root, &child, &rootX, &rootY, &x, &y,
                        &mask)) {
        return (mask & Button1Mask) != 0;
      }
    }
  }
#endif
  return trackedFallback;
}

void postNativeEscapeKey()
{
#if defined(DESKSEND_HAVE_X11) && defined(DESKSEND_HAVE_XTEST)
  withXTest([](Display *display) {
    if (const auto key = XKeysymToKeycode(display, XK_Escape)) {
      XTestFakeKeyEvent(display, key, True, CurrentTime);
      XTestFakeKeyEvent(display, key, False, CurrentTime);
    }
  });
#endif
}

void postNativeRelativeMouseMove([[maybe_unused]] int dx, [[maybe_unused]] int dy)
{
#if defined(DESKSEND_HAVE_X11) && defined(DESKSEND_HAVE_XTEST)
  withXTest([=](Display *display) { XTestFakeRelativeMotionEvent(display, dx, dy, CurrentTime); });
#endif
}

void postNativeLeftMouseUp()
{
#if defined(DESKSEND_HAVE_X11) && defined(DESKSEND_HAVE_XTEST)
  withXTest([](Display *display) { XTestFakeButtonEvent(display, 1, False, CurrentTime); });
#endif
}

std::string resolveNonWindowsDropTargetDirectory(
    [[maybe_unused]] int32_t x, [[maybe_unused]] int32_t y, const std::string &fallbackDir, bool *direct
)
{
  if (direct) {
    *direct = false;
  }
  if (const auto forced = qgetenv("DESKSEND_DEFAULT_DROP_DIR"); !forced.isEmpty()) {
    if (direct) {
      *direct = true;
    }
    return forced.toStdString();
  }
#if defined(DESKSEND_HAVE_X11)
  if (auto display = openXDisplay()) {
    Window root = 0;
    for (int screen = 0; screen < ScreenCount(display.get()); ++screen) {
      const Window candidate = RootWindow(display.get(), screen);
      Window actualRoot = 0, child = 0;
      int rootX = 0, rootY = 0, windowX = 0, windowY = 0;
      unsigned mask = 0;
      if (XQueryPointer(display.get(), candidate, &actualRoot, &child, &rootX, &rootY, &windowX, &windowY, &mask)) {
        root = candidate;
        break;
      }
    }
    XWindowAttributes rootAttrs{};
    if (root && XGetWindowAttributes(display.get(), root, &rootAttrs) && x >= rootAttrs.x && y >= rootAttrs.y &&
        x < rootAttrs.x + rootAttrs.width && y < rootAttrs.y + rootAttrs.height && hitsDesktop(display.get(), root, x, y)) {
      const auto desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
      if (!desktop.isEmpty()) {
        if (direct) {
          *direct = true;
        }
        return desktop.toStdString();
      }
    }
  }
#endif
  return fallbackDir;
}

void configurePlatformOverlayWidget(QWidget *widget, bool clickThrough)
{
  if (!widget) {
    return;
  }
  widget->setAttribute(Qt::WA_TransparentForMouseEvents, clickThrough);
  if (!widget->isVisible() && widget->windowFlags().testFlag(Qt::WindowTransparentForInput) != clickThrough) {
    widget->setWindowFlag(Qt::WindowTransparentForInput, clickThrough);
  }
}

} // namespace deskflow::platform_drop
