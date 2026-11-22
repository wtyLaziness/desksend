/* SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception */

#pragma once

#include <QByteArray>
#include <QList>
#include <QUrl>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class QWidget;

namespace deskflow::platform_drop {

struct EdgeSentinelCallbacks
{
  std::function<void(int sideIndex, std::vector<std::string> files, int32_t x, int32_t y)> onDragEnter;
  std::function<void()> onDragLeave;
  std::function<void(bool stillPressed)> onCancelComplete;
};

class QtEdgeSentinelHost
{
public:
  QtEdgeSentinelHost();
  ~QtEdgeSentinelHost();
  QtEdgeSentinelHost(const QtEdgeSentinelHost &) = delete;
  QtEdgeSentinelHost &operator=(const QtEdgeSentinelHost &) = delete;

  void setCallbacks(EdgeSentinelCallbacks callbacks);
  void ensureInitialized(int32_t x, int32_t y, int32_t w, int32_t h);
  void setEdgeGeometryAndVisible(std::size_t idx, int32_t rx, int32_t ry, int32_t rw, int32_t rh, bool show);
  void setEdgeVisible(std::size_t idx, bool show);
  void hideAll();
  void clearCancellationFlags();
  void cancelBeforeSwitch(int sideIndex);
  void shutdown();

private:
  struct Impl;
  std::shared_ptr<Impl> m_impl;
};

QList<QUrl> buildLocalFileUrls(const std::vector<std::string> &files);
std::vector<std::string> extractLocalFilePathsFromUrls(const QList<QUrl> &urls);
QByteArray buildGnomeCopiedFilesPayload(const std::vector<std::string> &files);
std::string parseFinderPathOutput(const std::string &stdoutText);
void copyFilesToPlatformClipboard(const std::vector<std::string> &files);

bool isNativeEdgeSentinelSupported();
bool isNativeLeftMouseButtonDown(bool trackedFallback);
void postNativeEscapeKey();
void postNativeRelativeMouseMove(int dx, int dy);
void postNativeLeftMouseUp();
std::string resolveNonWindowsDropTargetDirectory(
    int32_t screenX, int32_t screenY, const std::string &fallbackDir, bool *outIsDirectFolderTarget
);
void configurePlatformOverlayWidget(QWidget *widget, bool clickThrough);

} // namespace deskflow::platform_drop
