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
    CGRect hitBounds{};
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
      hitBounds = bounds;
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
          QStringLiteral("/usr/bin/osascript"),
          {QStringLiteral("-e"),
           QStringLiteral(
               "on run argv\n"
               "set hitBounds to {}\n"
               "repeat with coord in argv\n"
               "set end of hitBounds to (contents of coord as integer)\n"
               "end repeat\n"
               "tell application id \"com.apple.finder\"\n"
               "set matchingWindows to {}\n"
               "repeat with candidate in Finder windows\n"
               "if (bounds of candidate) is hitBounds then set end of matchingWindows to contents of candidate\n"
               "end repeat\n"
               "if (count of matchingWindows) is 1 then\n"
               "return POSIX path of (target of item 1 of matchingWindows as alias)\n"
               "end if\n"
               "end tell\n"
               "return \"\"\n"
               "end run"),
           QStringLiteral("--"), QString::number(qRound(CGRectGetMinX(hitBounds))),
           QString::number(qRound(CGRectGetMinY(hitBounds))), QString::number(qRound(CGRectGetMaxX(hitBounds))),
           QString::number(qRound(CGRectGetMaxY(hitBounds)))}
      );
      if (!finderQuery.waitForFinished(1500)) {
        if (finderQuery.state() != QProcess::NotRunning) {
          finderQuery.kill();
          finderQuery.waitForFinished();
        }
        return fallbackDir;
      }
      if (finderQuery.exitStatus() == QProcess::NormalExit && finderQuery.exitCode() == 0) {
        auto output = finderQuery.readAllStandardOutput();
        if (output.endsWith('\n')) {
          output.chop(1);
        }
        if (output.startsWith('/') && QFileInfo(QString::fromUtf8(output)).isDir()) {
          path = output.toStdString();
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
