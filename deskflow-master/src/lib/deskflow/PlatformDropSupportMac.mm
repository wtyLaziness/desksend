/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */

#include "deskflow/PlatformDropSupport.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QWidget>

#import <Cocoa/Cocoa.h>
#include <CoreGraphics/CoreGraphics.h>
#include <unistd.h>

namespace deskflow::platform_drop {
namespace {

void postEvent(CGEventRef event)
{
  if (event) {
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
  }
}

void postMouseEvent(CGEventType type, int dx, int dy)
{
  auto event = CGEventCreate(nullptr);
  if (!event) {
    return;
  }
  auto position = CGEventGetLocation(event);
  CFRelease(event);
  position.x += dx;
  position.y += dy;
  postEvent(CGEventCreateMouseEvent(nullptr, type, position, kCGMouseButtonLeft));
}

} // namespace

bool isNativeEdgeSentinelSupported() { return true; }

bool isNativeLeftMouseButtonDown(bool)
{
  return CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft);
}

void postNativeEscapeKey()
{
  constexpr CGKeyCode escape = 53;
  postEvent(CGEventCreateKeyboardEvent(nullptr, escape, true));
  postEvent(CGEventCreateKeyboardEvent(nullptr, escape, false));
}

void postNativeRelativeMouseMove(int dx, int dy) { postMouseEvent(kCGEventLeftMouseDragged, dx, dy); }
void postNativeLeftMouseUp() { postMouseEvent(kCGEventLeftMouseUp, 0, 0); }

std::string resolveNonWindowsDropTargetDirectory(int32_t x, int32_t y, const std::string &fallbackDir, bool *direct)
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

  @autoreleasepool {
    auto windowList = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!windowList) {
      return fallbackDir;
    }
    bool finder = false;
    bool desktop = false;
    for (NSDictionary *window in (__bridge NSArray *)windowList) {
      NSNumber *pid = window[(__bridge NSString *)kCGWindowOwnerPID];
      if ([pid intValue] == getpid()) {
        continue;
      }
      NSNumber *alpha = window[(__bridge NSString *)kCGWindowAlpha];
      if (alpha && [alpha doubleValue] <= 0.0) {
        continue;
      }
      auto boundsDict = (__bridge CFDictionaryRef)window[(__bridge NSString *)kCGWindowBounds];
      CGRect bounds;
      if (!boundsDict || !CGRectMakeWithDictionaryRepresentation(boundsDict, &bounds) ||
          !CGRectContainsPoint(bounds, CGPointMake(x, y))) {
        continue;
      }
      NSString *owner = window[(__bridge NSString *)kCGWindowOwnerName];
      finder = [owner isEqualToString:@"Finder"];
      NSNumber *layer = window[(__bridge NSString *)kCGWindowLayer];
      desktop = [layer intValue] <= CGWindowLevelForKey(kCGDesktopIconWindowLevelKey);
      break;
    }
    CFRelease(windowList);
    if (!finder) {
      return fallbackDir;
    }

    std::string path;
    if (desktop) {
      path = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation).toStdString();
    } else {
      QProcess finderQuery;
      finderQuery.start(
          QStringLiteral("osascript"),
          {QStringLiteral("-e"),
           QStringLiteral("tell application \"Finder\" to POSIX path of (target of front Finder window as alias)")}
      );
      if (!finderQuery.waitForFinished(1500)) {
        if (finderQuery.state() != QProcess::NotRunning) {
          finderQuery.kill();
          finderQuery.waitForFinished();
        }
        return fallbackDir;
      }
      if (finderQuery.exitStatus() == QProcess::NormalExit && finderQuery.exitCode() == 0) {
        path = parseFinderPathOutput(finderQuery.readAllStandardOutput().toStdString());
        if (!QFileInfo(QString::fromStdString(path)).isDir()) {
          path.clear();
        }
      }
    }
    if (!path.empty()) {
      if (direct) {
        *direct = true;
      }
      return path;
    }
  }
  return fallbackDir;
}

void configurePlatformOverlayWidget(QWidget *widget, bool clickThrough)
{
  if (!widget) {
    return;
  }
  @autoreleasepool {
    auto *view = reinterpret_cast<NSView *>(widget->winId());
    if (NSWindow *window = [view window]) {
      window.collectionBehavior |= NSWindowCollectionBehaviorCanJoinAllSpaces |
                                   NSWindowCollectionBehaviorFullScreenAuxiliary;
      window.ignoresMouseEvents = clickThrough;
    }
  }
}

} // namespace deskflow::platform_drop
