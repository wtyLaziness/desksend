/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 DeskSend Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>

#include <cstdint>
#include <memory>

namespace deskflow {

QString formatDragHoverText(uint32_t fileCount);

QPoint clampDragHoverPosition(
    const QPoint &cursorPos, const QSize &widgetSize, const QRect &screenBounds, const QPoint &offset = QPoint(18, 22)
);

class DragHoverIndicator
{
public:
  static constexpr int kAutoHideMs = 2500;

  DragHoverIndicator();
  ~DragHoverIndicator();
  DragHoverIndicator(const DragHoverIndicator &) = delete;
  DragHoverIndicator &operator=(const DragHoverIndicator &) = delete;

  void showHover(uint32_t fileCount, int32_t x, int32_t y);
  void updateCursor(int32_t x, int32_t y);
  void hideHover();

  bool isVisible() const;
  uint32_t fileCount() const;
  QPoint cursorPos() const;

private:
  struct Impl;
  std::shared_ptr<Impl> m_impl;
};

class TransferStatusOverlay
{
public:
  enum class Phase
  {
    Idle,
    Preparing,
    Active,
    Finished
  };

  explicit TransferStatusOverlay(bool receiving);
  ~TransferStatusOverlay();
  TransferStatusOverlay(const TransferStatusOverlay &) = delete;
  TransferStatusOverlay &operator=(const TransferStatusOverlay &) = delete;

  void beginPreparing(uint32_t fileCount);
  void cancelPreparing();
  void begin(const QString &description);
  void progress(uint64_t done, uint64_t total);
  void finish(bool success, const QString &message);

  Phase phase() const;
  uint32_t preparingFileCount() const;

private:
  struct Impl;
  std::shared_ptr<Impl> m_impl;
};

} // namespace deskflow
