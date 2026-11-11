/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/FileDropBridge.h"

#include "base/DirectionTypes.h"
#include "base/Log.h"
#include "common/Enums.h"
#include "io/StreamFilter.h"
#include "net/IDataSocket.h"
#include "server/BaseClientProxy.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QThread>

#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <exdisp.h>
#include <ole2.h>
#include <servprov.h>
#include <shellapi.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windows.h>
#endif

namespace deskflow {

namespace {

uint64_t nowMs()
{
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

#if defined(_WIN32)

std::string wideToUtf8(const std::wstring &w)
{
  if (w.empty()) {
    return {};
  }
  int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  if (len <= 0) {
    return {};
  }
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
  return out;
}

std::vector<std::string> extractFilesFromDataObject(IDataObject *obj)
{
  std::vector<std::string> results;
  if (!obj) {
    return results;
  }

  FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM med = {};
  if (SUCCEEDED(obj->GetData(&fmt, &med))) {
    auto *drop = static_cast<HDROP>(GlobalLock(med.hGlobal));
    if (drop) {
      UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < count; ++i) {
        wchar_t path[MAX_PATH * 2] = {};
        if (DragQueryFileW(drop, i, path, MAX_PATH * 2) > 0) {
          results.push_back(wideToUtf8(path));
        }
      }
      GlobalUnlock(med.hGlobal);
    }
    ReleaseStgMedium(&med);
    return results;
  }

  // Fallback: CFSTR_SHELLIDLIST
  FORMATETC idFmt = {
      static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL
  };
  if (SUCCEEDED(obj->GetData(&idFmt, &med))) {
    auto *cida = static_cast<CIDA *>(GlobalLock(med.hGlobal));
    if (cida) {
      auto parent =
          reinterpret_cast<PCUIDLIST_ABSOLUTE>(reinterpret_cast<const BYTE *>(cida) + cida->aoffset[0]);
      for (UINT i = 0; i < cida->cidl; ++i) {
        auto child =
            reinterpret_cast<PCUIDLIST_RELATIVE>(reinterpret_cast<const BYTE *>(cida) + cida->aoffset[i + 1]);
        PIDLIST_ABSOLUTE full = ILCombine(parent, child);
        if (full) {
          wchar_t path[MAX_PATH * 2] = {};
          if (SHGetPathFromIDListW(full, path)) {
            results.push_back(wideToUtf8(path));
          }
          ILFree(full);
        }
      }
      GlobalUnlock(med.hGlobal);
    }
    ReleaseStgMedium(&med);
  }
  return results;
}

class SentinelDropTarget : public IDropTarget
{
public:
  int sideIndex = -1; // 0:L, 1:R, 2:T, 3:B

  ULONG STDMETHODCALLTYPE AddRef() override
  {
    return 1;
  }
  ULONG STDMETHODCALLTYPE Release() override
  {
    return 1;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
  {
    if (riid == IID_IUnknown || riid == IID_IDropTarget) {
      *ppv = static_cast<IDropTarget *>(this);
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *obj, DWORD, POINTL pt, DWORD *eff) override
  {
    auto files = extractFilesFromDataObject(obj);
    if (!files.empty()) {
      LOG_INFO("FileDropBridge: sentinel DragEnter at (%ld,%ld) captured %zu file(s)", pt.x, pt.y, files.size());
      FileDropBridge::instance().setPendingDragFiles(std::move(files));
      *eff = DROPEFFECT_COPY;
      // Nudge cursor into the 1px jump zone so Server::onMouseMovePrimary switches immediately
      int dx = (sideIndex == 0) ? -8 : (sideIndex == 1 ? 8 : 0);
      int dy = (sideIndex == 2) ? -8 : (sideIndex == 3 ? 8 : 0);
      if (dx != 0 || dy != 0) {
        mouse_event(MOUSEEVENTF_MOVE, static_cast<DWORD>(dx), static_cast<DWORD>(dy), 0, 0);
      }
    } else {
      *eff = DROPEFFECT_NONE;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *eff) override
  {
    *eff = FileDropBridge::instance().hasPendingDragFiles() ? DROPEFFECT_COPY : DROPEFFECT_NONE;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragLeave() override
  {
    // Note: Do not clear pending files here because Deskflow's leave() warps
    // the cursor to screen center immediately upon switching screens.
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE Drop(IDataObject *obj, DWORD, POINTL pt, DWORD *eff) override
  {
    auto files = extractFilesFromDataObject(obj);
    if (!files.empty()) {
      LOG_INFO("FileDropBridge: sentinel Drop at (%ld,%ld) with %zu file(s)", pt.x, pt.y, files.size());
      FileDropBridge::instance().setPendingDragFiles(std::move(files));
      *eff = DROPEFFECT_COPY;
    } else {
      *eff = DROPEFFECT_NONE;
    }
    return S_OK;
  }
};

struct Win32SentinelManager
{
  static constexpr int kThickness = 8;
  static constexpr int kActivationZone = 36;

  bool initialized = false;
  int32_t lastX = 0;
  int32_t lastY = 0;
  int32_t lastW = 0;
  int32_t lastH = 0;
  std::array<HWND, 4> hwnds = {nullptr, nullptr, nullptr, nullptr}; // 0:L, 1:R, 2:T, 3:B
  std::array<bool, 4> visible = {false, false, false, false};
  std::array<SentinelDropTarget, 4> dropTargets;

  void ensureInitialized(int32_t x, int32_t y, int32_t w, int32_t h)
  {
    bool needUpdateGeometry = false;
    if (!initialized) {
      OleInitialize(nullptr);
      WNDCLASSW wc = {};
      wc.lpfnWndProc = DefWindowProcW;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"DeskflowFileDropSentinel";
      RegisterClassW(&wc);

      for (size_t i = 0; i < 4; ++i) {
        dropTargets[i].sideIndex = static_cast<int>(i);
        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, wc.lpszClassName, L"DeskflowSentinel",
            WS_POPUP, 0, 0, kThickness, kThickness, nullptr, nullptr, wc.hInstance, nullptr
        );
        if (hwnd) {
          SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
          RegisterDragDrop(hwnd, &dropTargets[i]);
          hwnds[i] = hwnd;
        }
      }
      initialized = true;
      needUpdateGeometry = true;
    } else if (x != lastX || y != lastY || w != lastW || h != lastH) {
      needUpdateGeometry = true;
    }

    if (needUpdateGeometry) {
      updateGeometry(x, y, w, h);
    }
  }

  void updateGeometry(int32_t x, int32_t y, int32_t w, int32_t h)
  {
    if (!initialized || w <= 0 || h <= 0) {
      return;
    }
    lastX = x;
    lastY = y;
    lastW = w;
    lastH = h;
    if (hwnds[0]) {
      SetWindowPos(hwnds[0], HWND_TOPMOST, x, y, kThickness, h, SWP_NOACTIVATE | (visible[0] ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
    if (hwnds[1]) {
      SetWindowPos(
          hwnds[1], HWND_TOPMOST, x + w - kThickness, y, kThickness, h,
          SWP_NOACTIVATE | (visible[1] ? SWP_SHOWWINDOW : SWP_HIDEWINDOW)
      );
    }
    if (hwnds[2]) {
      SetWindowPos(hwnds[2], HWND_TOPMOST, x, y, w, kThickness, SWP_NOACTIVATE | (visible[2] ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    }
    if (hwnds[3]) {
      SetWindowPos(
          hwnds[3], HWND_TOPMOST, x, y + h - kThickness, w, kThickness,
          SWP_NOACTIVATE | (visible[3] ? SWP_SHOWWINDOW : SWP_HIDEWINDOW)
      );
    }
  }

  void setEdgeVisible(size_t idx, bool show)
  {
    if (!initialized || idx >= 4 || !hwnds[idx] || visible[idx] == show) {
      return;
    }
    visible[idx] = show;
    if (show) {
      SetWindowPos(hwnds[idx], HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else {
      ShowWindow(hwnds[idx], SW_HIDE);
    }
  }

  void hideAll()
  {
    for (size_t i = 0; i < 4; ++i) {
      setEdgeVisible(i, false);
    }
  }

  void shutdown()
  {
    if (!initialized) {
      return;
    }
    for (size_t i = 0; i < 4; ++i) {
      if (hwnds[i]) {
        RevokeDragDrop(hwnds[i]);
        DestroyWindow(hwnds[i]);
        hwnds[i] = nullptr;
      }
      visible[i] = false;
    }
    lastW = 0;
    lastH = 0;
    initialized = false;
    OleUninitialize();
  }
};

Win32SentinelManager g_sentinelMgr;

void assignProcessToKillOnCloseJob(qint64 pid)
{
  if (pid <= 0) {
    return;
  }
  static HANDLE s_jobHandle = []() -> HANDLE {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
      jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
    }
    return job;
  }();
  if (!s_jobHandle) {
    return;
  }
  HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (hProc) {
    AssignProcessToJobObject(s_jobHandle, hProc);
    CloseHandle(hProc);
  }
}

void copyFilesToWindowsClipboard(const std::vector<std::string> &utf8Paths)
{
  if (utf8Paths.empty()) {
    return;
  }
  std::vector<wchar_t> buffer;
  for (const auto &p : utf8Paths) {
    QString q = QDir::toNativeSeparators(QString::fromStdString(p));
    std::wstring w = q.toStdWString();
    if (!w.empty()) {
      buffer.insert(buffer.end(), w.begin(), w.end());
      buffer.push_back(L'\0');
    }
  }
  if (buffer.empty()) {
    return;
  }
  buffer.push_back(L'\0');

  const SIZE_T bytes = sizeof(DROPFILES) + buffer.size() * sizeof(wchar_t);
  HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (!hMem) {
    return;
  }
  auto *df = static_cast<DROPFILES *>(GlobalLock(hMem));
  if (!df) {
    GlobalFree(hMem);
    return;
  }
  df->pFiles = sizeof(DROPFILES);
  df->fWide = TRUE;
  std::memcpy(reinterpret_cast<BYTE *>(df) + sizeof(DROPFILES), buffer.data(), buffer.size() * sizeof(wchar_t));
  GlobalUnlock(hMem);

  if (OpenClipboard(nullptr)) {
    EmptyClipboard();
    if (!SetClipboardData(CF_HDROP, hMem)) {
      GlobalFree(hMem);
    } else {
      LOG_INFO("FileDropBridge: copied %zu received file(s) to system clipboard (CF_HDROP) for non-Explorer target", utf8Paths.size());
    }
    CloseClipboard();
  } else {
    GlobalFree(hMem);
  }
}

QString resolveHostAllowSpec(const QString &hostSpec)
{
  if (hostSpec.isEmpty() || hostSpec == QStringLiteral("localhost")) {
    return QStringLiteral("127.0.0.1");
  }
  if (hostSpec == QStringLiteral("*")) {
    return hostSpec;
  }
  QStringList parts;
  parts << hostSpec;

  addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *res = nullptr;
  const std::string hostUtf8 = hostSpec.toStdString();
  if (getaddrinfo(hostUtf8.c_str(), nullptr, &hints, &res) == 0 && res) {
    for (addrinfo *p = res; p != nullptr; p = p->ai_next) {
      char ipBuf[INET6_ADDRSTRLEN] = {};
      if (p->ai_family == AF_INET) {
        auto *sin = reinterpret_cast<sockaddr_in *>(p->ai_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, ipBuf, sizeof(ipBuf))) {
          const QString ipStr = QString::fromLatin1(ipBuf);
          if (!parts.contains(ipStr)) {
            parts << ipStr;
          }
        }
      } else if (p->ai_family == AF_INET6) {
        auto *sin6 = reinterpret_cast<sockaddr_in6 *>(p->ai_addr);
        if (inet_ntop(AF_INET6, &sin6->sin6_addr, ipBuf, sizeof(ipBuf))) {
          QString ipStr = QString::fromLatin1(ipBuf);
          if (ipStr.startsWith(QStringLiteral("::ffff:"), Qt::CaseInsensitive)) {
            ipStr = ipStr.mid(7);
          }
          if (!parts.contains(ipStr)) {
            parts << ipStr;
          }
        }
      }
    }
    freeaddrinfo(res);
  }
  return parts.join(',');
}

#endif // _WIN32

void cleanEmptySubdirsInStaging(const QString &dirPath, bool isRoot)
{
  QDir dir(dirPath);
  if (!dir.exists()) {
    return;
  }
  const QFileInfoList subDirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
  for (const QFileInfo &sub : subDirs) {
    cleanEmptySubdirsInStaging(sub.absoluteFilePath(), false);
  }
  if (!isRoot && dir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty()) {
    QDir parent = dir;
    if (parent.cdUp()) {
      parent.rmdir(dir.dirName());
    }
  }
}

} // namespace

FileDropBridge &FileDropBridge::instance()
{
  static FileDropBridge s_instance;
  return s_instance;
}

FileDropBridge::~FileDropBridge()
{
  stopReceiver();
  shutdownSentinels();
}

void FileDropBridge::updatePrimaryShape(int32_t x, int32_t y, int32_t w, int32_t h)
{
#if defined(_WIN32)
  g_sentinelMgr.ensureInitialized(x, y, w, h);
#else
  (void)x;
  (void)y;
  (void)w;
  (void)h;
#endif
}

void FileDropBridge::shutdownSentinels()
{
#if defined(_WIN32)
  g_sentinelMgr.shutdown();
#endif
  std::scoped_lock lock(m_mutex);
  m_pendingFiles.clear();
  m_activeDragFiles.clear();
  m_dragTargetClient = nullptr;
}

void FileDropBridge::onPrimaryMouseMove(
    int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t aw, int32_t ah, uint32_t activeSides
)
{
#if defined(_WIN32)
  g_sentinelMgr.ensureInitialized(ax, ay, aw, ah);
  const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
  if (!leftButtonDown) {
    g_sentinelMgr.hideAll();
    std::scoped_lock lock(m_mutex);
    if (!m_pendingFiles.empty() && (nowMs() - m_pendingTimestampMs > 500)) {
      m_pendingFiles.clear();
    }
    return;
  }

  using enum DirectionMask;
  const int32_t zone = Win32SentinelManager::kActivationZone;
  const bool nearLeft = ((activeSides & static_cast<uint32_t>(LeftMask)) != 0) && (x < ax + zone);
  const bool nearRight = ((activeSides & static_cast<uint32_t>(RightMask)) != 0) && (x >= ax + aw - zone);
  const bool nearTop = ((activeSides & static_cast<uint32_t>(TopMask)) != 0) && (y < ay + zone);
  const bool nearBottom = ((activeSides & static_cast<uint32_t>(BottomMask)) != 0) && (y >= ay + ah - zone);

  g_sentinelMgr.setEdgeVisible(0, nearLeft);
  g_sentinelMgr.setEdgeVisible(1, nearRight);
  g_sentinelMgr.setEdgeVisible(2, nearTop);
  g_sentinelMgr.setEdgeVisible(3, nearBottom);
#else
  (void)x;
  (void)y;
  (void)ax;
  (void)ay;
  (void)aw;
  (void)ah;
  (void)activeSides;
#endif
}

void FileDropBridge::setPendingDragFiles(std::vector<std::string> files)
{
  std::scoped_lock lock(m_mutex);
  m_pendingFiles = std::move(files);
  m_pendingTimestampMs = nowMs();
}

bool FileDropBridge::hasPendingDragFiles() const
{
  std::scoped_lock lock(m_mutex);
  if (m_pendingFiles.empty()) {
    return false;
  }
  return (nowMs() - m_pendingTimestampMs) <= 4000;
}

void FileDropBridge::onLeavePrimary(BaseClientProxy *dstClient)
{
#if defined(_WIN32)
  g_sentinelMgr.hideAll();
#endif

  std::vector<std::string> files;
  {
    std::scoped_lock lock(m_mutex);
    if (m_pendingFiles.empty() || (nowMs() - m_pendingTimestampMs) > 4000) {
      m_pendingFiles.clear();
      return;
    }
    files = std::move(m_pendingFiles);
    m_pendingFiles.clear();
    m_activeDragFiles = files;
    m_dragTargetClient = dstClient;
  }

#if defined(_WIN32)
  // Cancel local OLE DoDragDrop loop on the source machine by injecting VK_ESCAPE with magic extra info
  // (0x44534B54) so MSWindowsHook::keyboardLLHook passes it to the local OS without relaying to Server.
  keybd_event(VK_ESCAPE, 0, 0, static_cast<ULONG_PTR>(0x44534B54));
  keybd_event(VK_ESCAPE, 0, KEYEVENTF_KEYUP, static_cast<ULONG_PTR>(0x44534B54));
#endif

  if (dstClient) {
    std::string msg = "DESKSEND_START";
    for (const auto &f : files) {
      msg += "\n";
      msg += QFileInfo(QString::fromStdString(f)).fileName().toStdString();
    }
    LOG_INFO(
        "FileDropBridge: leaving primary with %zu dragged file(s) to \"%s\"", files.size(),
        dstClient->getName().c_str()
    );
    dstClient->sendDragInfo(static_cast<uint32_t>(files.size()), msg.data(), msg.size());
  }
}

void FileDropBridge::onReturnToPrimary()
{
  BaseClientProxy *oldTarget = nullptr;
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeDragFiles.empty()) {
      return;
    }
#if defined(_WIN32)
    const bool stillHoldingLeft = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
    const bool stillHoldingLeft = false;
#endif
    if (stillHoldingLeft) {
      LOG_INFO("FileDropBridge: cursor returned to primary while LBUTTON still held; preserving pending drag files");
      m_pendingFiles = std::move(m_activeDragFiles);
      m_pendingTimestampMs = nowMs();
    } else {
      LOG_INFO("FileDropBridge: cursor returned to primary before release; cancelling file drag");
    }
    m_activeDragFiles.clear();
    oldTarget = m_dragTargetClient;
    m_dragTargetClient = nullptr;
  }
  if (oldTarget) {
    const std::string cancelMsg = "DESKSEND_CANCEL";
    oldTarget->sendDragInfo(0, cancelMsg.data(), cancelMsg.size());
  }
}

void FileDropBridge::onClientDisconnected(BaseClientProxy *client)
{
  if (!client) {
    return;
  }
  std::scoped_lock lock(m_mutex);
  if (m_dragTargetClient == client) {
    LOG_INFO("FileDropBridge: active drag target client disconnected; clearing target pointer");
    m_dragTargetClient = nullptr;
    m_activeDragFiles.clear();
  }
}

void FileDropBridge::cancelActiveDrag()
{
  BaseClientProxy *oldTarget = nullptr;
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeDragFiles.empty()) {
      return;
    }
    LOG_INFO("FileDropBridge: Escape pressed; cancelling file drag");
    m_activeDragFiles.clear();
    oldTarget = m_dragTargetClient;
    m_dragTargetClient = nullptr;
  }
  if (oldTarget) {
    const std::string cancelMsg = "DESKSEND_CANCEL";
    oldTarget->sendDragInfo(0, cancelMsg.data(), cancelMsg.size());
  }
}

void FileDropBridge::onSecondaryLeftMouseUp(BaseClientProxy *activeClient, int32_t x, int32_t y)
{
  std::vector<std::string> files;
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeDragFiles.empty() || activeClient == nullptr) {
      return;
    }
    files = std::move(m_activeDragFiles);
    m_activeDragFiles.clear();
    m_dragTargetClient = nullptr;
  }

  std::string dropMsg = "DESKSEND_DROP:" + std::to_string(x) + "," + std::to_string(y);
  activeClient->sendDragInfo(static_cast<uint32_t>(files.size()), dropMsg.data(), dropMsg.size());

  std::string peerIp = getClientPeerIp(activeClient);
  if (peerIp.empty()) {
    LOG_WARN("FileDropBridge: could not determine peer IP for client \"%s\"", activeClient->getName().c_str());
    return;
  }

  LOG_INFO(
      "FileDropBridge: drop at (%d,%d) on \"%s\" (%s), sending %zu file(s) via LocalSend", x, y,
      activeClient->getName().c_str(), peerIp.c_str(), files.size()
  );
  sendFilesViaLocalSend(peerIp, files);
}

std::string FileDropBridge::getClientPeerIp(BaseClientProxy *client)
{
  if (!client) {
    return {};
  }
  deskflow::IStream *s = client->getStream();
  while (auto *filter = dynamic_cast<StreamFilter *>(s)) {
    s = filter->getStream();
  }
  auto *dataSocket = dynamic_cast<IDataSocket *>(s);
  if (!dataSocket) {
    return {};
  }
  std::string addr = dataSocket->getPeerAddress();
  if (addr.empty()) {
    return {};
  }
  // Strip port if formatted as "ip:port" or "[ipv6]:port"
  if (addr.front() == '[') {
    auto closeBracket = addr.find(']');
    if (closeBracket != std::string::npos) {
      addr = addr.substr(1, closeBracket - 1);
    }
  } else {
    auto firstColon = addr.find(':');
    auto lastColon = addr.rfind(':');
    if (firstColon != std::string::npos && firstColon == lastColon) {
      addr = addr.substr(0, firstColon);
    }
  }
  const QString qAddr = QString::fromStdString(addr);
  if (qAddr.startsWith(QStringLiteral("::ffff:"), Qt::CaseInsensitive)) {
    return qAddr.mid(7).toStdString();
  }
  return addr;
}

std::string FileDropBridge::findLocalSendCliPath()
{
  if (const char *envPath = std::getenv("DESKSEND_LOCALSEND_CLI"); envPath && envPath[0] != '\0') {
    if (QFileInfo::exists(QString::fromUtf8(envPath))) {
      return envPath;
    }
  }

  const QString appDir = QCoreApplication::applicationDirPath();
#if defined(_WIN32)
  const QString exeName = QStringLiteral("localsend-cli.exe");
#else
  const QString exeName = QStringLiteral("localsend-cli");
#endif
  const QString candidate = QDir(appDir).filePath(exeName);
  if (QFileInfo::exists(candidate)) {
    return candidate.toStdString();
  }
  return exeName.toStdString();
}

void FileDropBridge::sendFilesViaLocalSend(const std::string &targetIp, const std::vector<std::string> &files)
{
  if (auto *app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
    QMetaObject::invokeMethod(
        app, [this, targetIp, files]() { sendFilesViaLocalSend(targetIp, files); }, Qt::QueuedConnection
    );
    return;
  }

  {
    std::scoped_lock lock(m_mutex);
    m_sendQueue.push_back(SendJob{targetIp, files});
    if (m_sendingInProgress) {
      LOG_INFO("FileDropBridge: queued send job to %s (%zu file(s)); %zu job(s) waiting", targetIp.c_str(), files.size(), m_sendQueue.size());
      return;
    }
    m_sendingInProgress = true;
  }
  processNextSendJob();
}

void FileDropBridge::processNextSendJob()
{
  SendJob job;
  {
    std::scoped_lock lock(m_mutex);
    if (m_sendQueue.empty()) {
      m_sendingInProgress = false;
      return;
    }
    job = std::move(m_sendQueue.front());
    m_sendQueue.pop_front();
  }

  const QString cliPath = QString::fromStdString(findLocalSendCliPath());
  QString targetPortStr = QStringLiteral("53318");
  if (const char *targetPort = std::getenv("DESKSEND_TARGET_PORT"); targetPort && targetPort[0] != '\0') {
    targetPortStr = QString::fromUtf8(targetPort);
  }
  const QString destination = QString::fromStdString(job.targetIp) + QStringLiteral(":") + targetPortStr;

  QStringList args;
  if (const char *senderPort = std::getenv("DESKSEND_SENDER_PORT"); senderPort && senderPort[0] != '\0') {
    args << QStringLiteral("--port") << QString::fromUtf8(senderPort);
  } else {
    // Use dynamic OS-assigned port (0) so concurrent or rapid consecutive sends never conflict
    args << QStringLiteral("--port") << QStringLiteral("0");
  }
  args << QStringLiteral("send") << QStringLiteral("--to") << destination;
  for (const auto &f : job.files) {
    args << QString::fromStdString(f);
  }

  auto *proc = new QProcess();
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  QString senderCfgDir = QDir(QDir::tempPath()).filePath(QStringLiteral("DeskSend_SenderCfg"));
  if (const char *senderCfg = std::getenv("DESKSEND_SENDER_CONFIG_HOME"); senderCfg && senderCfg[0] != '\0') {
    senderCfgDir = QString::fromUtf8(senderCfg);
  }
  QDir().mkpath(senderCfgDir);
  env.insert(QStringLiteral("XDG_CONFIG_HOME"), senderCfgDir);
  proc->setProcessEnvironment(env);

#if defined(_WIN32)
  QObject::connect(proc, &QProcess::started, proc, [proc]() { assignProcessToKillOnCloseJob(proc->processId()); });
#endif

  QObject::connect(proc, &QProcess::finished, proc, [this, proc, destination](int exitCode, QProcess::ExitStatus) {
    const QString out = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
    if (exitCode == 0) {
      LOG_INFO("FileDropBridge: LocalSend transfer to %s succeeded", destination.toStdString().c_str());
    } else {
      LOG_WARN(
          "FileDropBridge: LocalSend transfer to %s failed (exit=%d) out=\"%s\" err=\"%s\"",
          destination.toStdString().c_str(), exitCode, out.toStdString().c_str(), err.toStdString().c_str()
      );
    }
    proc->deleteLater();
    processNextSendJob();
  });

  LOG_INFO("FileDropBridge: spawning %s %s", cliPath.toStdString().c_str(), args.join(' ').toStdString().c_str());
  proc->start(cliPath, args);
}

void FileDropBridge::startReceiver(const std::string &serverHost)
{
  if (auto *app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
    QMetaObject::invokeMethod(app, [this, serverHost]() { startReceiver(serverHost); }, Qt::QueuedConnection);
    return;
  }

  std::scoped_lock lock(m_mutex);
  if (m_receiverProcess && m_receiverProcess->state() != QProcess::NotRunning) {
    if (m_currentAllowedServerIp == serverHost) {
      return;
    }
    m_receiverProcess->kill();
    m_receiverProcess->waitForFinished(1000);
    delete m_receiverProcess;
    m_receiverProcess = nullptr;
  }

  m_currentAllowedServerIp = serverHost;
  QString staging = QDir(QDir::tempPath()).filePath(QStringLiteral("DeskSend_Staging"));
  QDir().mkpath(staging);
  m_stagingDir = staging.toStdString();

  QString defaultDrop = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (defaultDrop.isEmpty()) {
    defaultDrop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
  }
  if (const char *envDrop = std::getenv("DESKSEND_DEFAULT_DROP_DIR"); envDrop && envDrop[0] != '\0') {
    defaultDrop = QString::fromUtf8(envDrop);
    QDir().mkpath(defaultDrop);
  }
  if (m_lastResolvedDropDir.empty()) {
    m_lastResolvedDropDir = defaultDrop.toStdString();
  }

  const QString cliPath = QString::fromStdString(findLocalSendCliPath());
  QString port = QStringLiteral("53318");
  if (const char *envPort = std::getenv("DESKSEND_RECEIVER_PORT"); envPort && envPort[0] != '\0') {
    port = QString::fromUtf8(envPort);
  }

  QString allowSpec = QString::fromStdString(serverHost);
  if (allowSpec.startsWith(QStringLiteral("::ffff:"), Qt::CaseInsensitive)) {
    allowSpec = allowSpec.mid(7);
  }
#if defined(_WIN32)
  allowSpec = resolveHostAllowSpec(allowSpec);
#else
  if (allowSpec.isEmpty() || allowSpec == QStringLiteral("localhost")) {
    allowSpec = QStringLiteral("127.0.0.1");
  }
#endif

  QStringList args;
  args << QStringLiteral("--port") << port << QStringLiteral("--destination") << staging << QStringLiteral("receive")
       << QStringLiteral("--auto-accept-from") << allowSpec;

  m_receiverProcess = new QProcess();
  m_receiverProcess->setProcessChannelMode(QProcess::MergedChannels);
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  QString rxCfgDir = QDir(QDir::tempPath()).filePath(QStringLiteral("DeskSend_ReceiverCfg"));
  if (const char *rxCfg = std::getenv("DESKSEND_RECEIVER_CONFIG_HOME"); rxCfg && rxCfg[0] != '\0') {
    rxCfgDir = QString::fromUtf8(rxCfg);
  }
  QDir().mkpath(rxCfgDir);
  env.insert(QStringLiteral("XDG_CONFIG_HOME"), rxCfgDir);
  m_receiverProcess->setProcessEnvironment(env);

#if defined(_WIN32)
  QObject::connect(m_receiverProcess, &QProcess::started, m_receiverProcess, [this]() {
    if (m_receiverProcess) {
      assignProcessToKillOnCloseJob(m_receiverProcess->processId());
    }
  });
#endif

  QObject::connect(m_receiverProcess, &QProcess::readyReadStandardOutput, [this]() {
    if (!m_receiverProcess) {
      return;
    }
    while (m_receiverProcess->canReadLine()) {
      const QString line = QString::fromUtf8(m_receiverProcess->readLine()).trimmed();
      LOG_INFO("FileDropBridge [rx]: %s", line.toStdString().c_str());
      const QString filePrefix = QStringLiteral("RECEIVED_FILE:");
      if (line.startsWith(filePrefix)) {
        handleReceivedFileLine(line.mid(filePrefix.size()).toStdString());
      } else if (line.startsWith(QStringLiteral("RECEIVE_SESSION_STARTED:"))) {
        handleReceiveSessionStarted();
      } else if (line.startsWith(QStringLiteral("RECEIVE_SESSION_FINISHED:")) ||
                 line.startsWith(QStringLiteral("RECEIVE_SESSION_CANCELLED:"))) {
        handleReceiveSessionFinished();
      }
    }
  });

  LOG_INFO(
      "FileDropBridge: starting headless receiver: %s %s", cliPath.toStdString().c_str(),
      args.join(' ').toStdString().c_str()
  );
  m_receiverProcess->start(cliPath, args);
}

void FileDropBridge::stopReceiver()
{
  if (auto *app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
    QMetaObject::invokeMethod(app, [this]() { stopReceiver(); }, Qt::QueuedConnection);
    return;
  }

  std::scoped_lock lock(m_mutex);
  if (m_receiverProcess) {
    m_receiverProcess->kill();
    m_receiverProcess->waitForFinished(1000);
    delete m_receiverProcess;
    m_receiverProcess = nullptr;
  }
}

void FileDropBridge::onClientReceiveDragInfo(uint16_t fileCount, const std::string &info)
{
  const std::string dropPrefix = "DESKSEND_DROP:";
  if (info.rfind(dropPrefix, 0) == 0) {
    const std::string coords = info.substr(dropPrefix.size());
    auto comma = coords.find(',');
    if (comma != std::string::npos) {
      int32_t x = std::atoi(coords.substr(0, comma).c_str());
      int32_t y = std::atoi(coords.substr(comma + 1).c_str());
      std::string fallback;
      {
        std::scoped_lock lock(m_mutex);
        fallback = m_lastResolvedDropDir;
      }
      bool isDirectFolder = true;
      std::string resolved = resolveDropTargetDirectory(x, y, fallback, &isDirectFolder);
      {
        std::scoped_lock lock(m_mutex);
        m_lastResolvedDropDir = resolved;
        m_pendingDropTargets.push_back(DropTargetEntry{resolved, !isDirectFolder});
      }
      LOG_INFO(
          "FileDropBridge: remote drop at (%d,%d) for %u file(s) -> target folder \"%s\" (directFolder=%d)", x, y,
          fileCount, resolved.c_str(), isDirectFolder ? 1 : 0
      );
    }
  } else if (info.rfind("DESKSEND_START", 0) == 0) {
    LOG_INFO("FileDropBridge: incoming drag started (%u file(s))", fileCount);
  } else if (info.rfind("DESKSEND_CANCEL", 0) == 0) {
    LOG_INFO("FileDropBridge: incoming drag cancelled");
  }
}

void FileDropBridge::handleReceiveSessionStarted()
{
  std::scoped_lock lock(m_mutex);
  m_activeSessionPlacedFiles.clear();
  if (!m_pendingDropTargets.empty()) {
    m_activeSessionTarget = std::move(m_pendingDropTargets.front());
    m_pendingDropTargets.pop_front();
  } else {
    m_activeSessionTarget = DropTargetEntry{m_lastResolvedDropDir, false};
  }
}

void FileDropBridge::handleReceiveSessionFinished()
{
  std::vector<std::string> filesForClipboard;
  bool shouldCopyToClipboard = false;
  std::string stagingDirStr;
  {
    std::scoped_lock lock(m_mutex);
    shouldCopyToClipboard = m_activeSessionTarget.copyToClipboard;
    if (shouldCopyToClipboard) {
      filesForClipboard = std::move(m_activeSessionPlacedFiles);
    }
    m_activeSessionPlacedFiles.clear();
    m_activeSessionTarget = DropTargetEntry{};
    stagingDirStr = m_stagingDir;
  }

#if defined(_WIN32)
  if (shouldCopyToClipboard && !filesForClipboard.empty()) {
    copyFilesToWindowsClipboard(filesForClipboard);
  }
#else
  (void)shouldCopyToClipboard;
  (void)filesForClipboard;
#endif

  if (!stagingDirStr.empty()) {
    cleanEmptySubdirsInStaging(QString::fromStdString(stagingDirStr), true);
  }
}

void FileDropBridge::handleReceivedFileLine(const std::string &filePath)
{
  std::string targetDir;
  std::string stagingDirStr;
  {
    std::scoped_lock lock(m_mutex);
    if (!m_activeSessionTarget.dir.empty()) {
      targetDir = m_activeSessionTarget.dir;
    } else if (!m_pendingDropTargets.empty()) {
      targetDir = m_pendingDropTargets.front().dir;
    } else {
      targetDir = m_lastResolvedDropDir;
    }
    stagingDirStr = m_stagingDir;
  }
  if (targetDir.empty()) {
    return;
  }

  QFileInfo srcInfo(QString::fromStdString(filePath));
  if (!srcInfo.exists()) {
    return;
  }

  QDir dstRootDir(QString::fromStdString(targetDir));
  if (!dstRootDir.exists()) {
    dstRootDir.mkpath(QStringLiteral("."));
  }

  QString relPath = srcInfo.fileName();
  if (!stagingDirStr.empty()) {
    QDir stagingDir(QString::fromStdString(stagingDirStr));
    const QString candidateRel = stagingDir.relativeFilePath(srcInfo.absoluteFilePath());
    if (!candidateRel.isEmpty() && !candidateRel.startsWith(QStringLiteral("..")) &&
        !QDir::isAbsolutePath(candidateRel)) {
      relPath = candidateRel;
    }
  }

  QString dstPath = dstRootDir.filePath(relPath);
  QFileInfo dstInfo(dstPath);
  QDir dstParentDir = dstInfo.dir();
  if (!dstParentDir.exists()) {
    dstParentDir.mkpath(QStringLiteral("."));
  }

  QString baseName = dstInfo.completeBaseName();
  QString suffix = dstInfo.suffix();
  int counter = 1;
  while (QFileInfo::exists(dstPath)) {
    if (suffix.isEmpty()) {
      dstPath = dstParentDir.filePath(QStringLiteral("%1 (%2)").arg(baseName).arg(counter));
    } else {
      dstPath = dstParentDir.filePath(QStringLiteral("%1 (%2).%3").arg(baseName).arg(counter).arg(suffix));
    }
    ++counter;
  }

  bool placed = false;
  if (QFile::rename(srcInfo.absoluteFilePath(), dstPath)) {
    LOG_INFO("FileDropBridge: placed received file at \"%s\"", dstPath.toStdString().c_str());
    placed = true;
  } else if (QFile::copy(srcInfo.absoluteFilePath(), dstPath)) {
    QFile::remove(srcInfo.absoluteFilePath());
    LOG_INFO("FileDropBridge: copied received file to \"%s\"", dstPath.toStdString().c_str());
    placed = true;
  } else {
    LOG_WARN(
        "FileDropBridge: failed to move \"%s\" to \"%s\"", filePath.c_str(), dstPath.toStdString().c_str()
    );
  }

  if (placed) {
    std::scoped_lock lock(m_mutex);
    m_activeSessionPlacedFiles.push_back(dstPath.toStdString());
  }
}

std::string FileDropBridge::resolveDropTargetDirectory(
    int32_t screenX, int32_t screenY, const std::string &fallbackDir, bool *outIsDirectFolderTarget
)
{
  if (outIsDirectFolderTarget) {
    *outIsDirectFolderTarget = true;
  }
#if defined(_WIN32)
  if (const char *forcedDir = std::getenv("DESKSEND_DEFAULT_DROP_DIR"); forcedDir && forcedDir[0] != '\0') {
    return forcedDir;
  }

  struct ComScope
  {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))
    {
    }
    ~ComScope()
    {
      if (SUCCEEDED(hr)) {
        CoUninitialize();
      }
    }
  } comScope;

  POINT pt = {screenX, screenY};
  HWND hwndHit = WindowFromPoint(pt);
  if (!hwndHit) {
    if (outIsDirectFolderTarget) {
      *outIsDirectFolderTarget = false;
    }
    return fallbackDir;
  }

  HWND hwndTop = GetAncestor(hwndHit, GA_ROOT);
  wchar_t topClass[128] = {};
  if (hwndTop) {
    GetClassNameW(hwndTop, topClass, 128);
  }

  // Case 1: Desktop (Progman / WorkerW)
  if (wcscmp(topClass, L"Progman") == 0 || wcscmp(topClass, L"WorkerW") == 0) {
    PWSTR desktopPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktopPath)) && desktopPath) {
      std::string res = wideToUtf8(desktopPath);
      CoTaskMemFree(desktopPath);
      if (!res.empty()) {
        return res;
      }
    }
  }

  // Case 2: Explorer window (CabinetWClass / ExploreWClass)
  if (wcscmp(topClass, L"CabinetWClass") == 0 || wcscmp(topClass, L"ExploreWClass") == 0) {
    IShellWindows *shellWindows = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&shellWindows))) &&
        shellWindows) {
      long count = 0;
      shellWindows->get_Count(&count);
      std::string matchedFolder;

      for (long i = 0; i < count; ++i) {
        VARIANT idx;
        VariantInit(&idx);
        idx.vt = VT_I4;
        idx.lVal = i;
        IDispatch *disp = nullptr;
        if (SUCCEEDED(shellWindows->Item(idx, &disp)) && disp) {
          IWebBrowserApp *webApp = nullptr;
          if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&webApp))) && webApp) {
            SHANDLE_PTR browserHwnd = 0;
            webApp->get_HWND(&browserHwnd);
            if (reinterpret_cast<HWND>(browserHwnd) == hwndTop) {
              IServiceProvider *sp = nullptr;
              if (SUCCEEDED(webApp->QueryInterface(IID_PPV_ARGS(&sp))) && sp) {
                IShellBrowser *shellBrowser = nullptr;
                if (SUCCEEDED(sp->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shellBrowser))) && shellBrowser) {
                  HWND tabHwnd = nullptr;
                  shellBrowser->GetWindow(&tabHwnd);
                  const bool isActiveOrHitTab =
                      !tabHwnd || tabHwnd == hwndHit || IsChild(tabHwnd, hwndHit) || IsWindowVisible(tabHwnd);
                  if (isActiveOrHitTab) {
                    IShellView *shellView = nullptr;
                    if (SUCCEEDED(shellBrowser->QueryActiveShellView(&shellView)) && shellView) {
                      IFolderView *folderView = nullptr;
                      if (SUCCEEDED(shellView->QueryInterface(IID_PPV_ARGS(&folderView))) && folderView) {
                        IPersistFolder2 *persistFolder = nullptr;
                        if (SUCCEEDED(folderView->GetFolder(IID_PPV_ARGS(&persistFolder))) && persistFolder) {
                          PIDLIST_ABSOLUTE pidl = nullptr;
                          if (SUCCEEDED(persistFolder->GetCurFolder(&pidl)) && pidl) {
                            wchar_t path[MAX_PATH * 2] = {};
                            if (SHGetPathFromIDListW(pidl, path)) {
                              matchedFolder = wideToUtf8(path);
                            }
                            CoTaskMemFree(pidl);
                          }
                          persistFolder->Release();
                        }
                        folderView->Release();
                      }
                      shellView->Release();
                    }
                  }
                  shellBrowser->Release();
                }
                sp->Release();
              }
            }
            webApp->Release();
          }
          disp->Release();
        }
        if (!matchedFolder.empty()) {
          break;
        }
      }
      shellWindows->Release();
      if (!matchedFolder.empty()) {
        return matchedFolder;
      }
    }
  }
  if (outIsDirectFolderTarget) {
    *outIsDirectFolderTarget = false;
  }
#else
  (void)screenX;
  (void)screenY;
#endif
  return fallbackDir;
}

} // namespace deskflow
