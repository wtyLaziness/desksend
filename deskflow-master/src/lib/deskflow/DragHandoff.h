// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <algorithm>
#include <cstdint>

namespace deskflow {
inline int32_t fileDragEntryCoordinate(int32_t coordinate, int32_t origin, int32_t extent)
{
  if (extent <= 1)
    return origin;
  // 文件拖拽取消 OLE 后可能残留小幅反向位移；落在边缘内侧，避免立刻回切取消。
  const int32_t inset = std::min<int32_t>(32, (extent - 1) / 2);
  return std::clamp(coordinate, origin + inset, origin + extent - 1 - inset);
}
}
