/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/FileDropBridge.h"

#include "base/DirectionTypes.h"
#include "base/Log.h"
#include "common/Enums.h"
#include "common/Settings.h"
#include "io/StreamFilter.h"
#include "net/IDataSocket.h"
#include "server/BaseClientProxy.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QLabel>
#include <QLocale>
#include <QProgressBar>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <array>
#include <atomic>
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

// 所有状态窗口操作均在 Qt 主线程执行，不抢走拖拽和键鼠输入的焦点。
class TransferStatus : public QWidget
{
public:
  explicit TransferStatus(bool receiving) : QWidget(nullptr, Qt::Tool | Qt::WindowStaysOnTopHint), m_receiving(receiving)
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
    m_hide.setSingleShot(true);
    QObject::connect(&m_hide, &QTimer::timeout, this, &QWidget::hide);
  }

  void begin(const QString &description)
  {
    m_hide.stop();
    m_elapsed.start();
    m_done = 0;
    m_total = 0;
    m_title->setText(description);
    m_bar->setRange(0, 0);
    m_details->setText(QCoreApplication::translate("FileDropBridge", "Preparing transfer..."));
    adjustSize();
    if (auto *screen = QGuiApplication::primaryScreen()) {
      const auto area = screen->availableGeometry();
      move(area.right() - width() - 20, area.bottom() - height() - (m_receiving ? 200 : 20));
    }
    show();
  }

  void progress(quint64 done, quint64 total)
  {
    m_done = qMin(done, total);
    m_total = total;
    m_bar->setRange(0, 100);
    m_bar->setValue(total ? static_cast<int>(100.0 * m_done / total) : 0);
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

  void finish(bool success, const QString &message)
  {
    if (success) {
      progress(m_total, m_total);
      m_bar->setValue(100);
    } else {
      m_bar->setRange(0, 100);
    }
    m_title->setText(message);
    if (!success) {
      m_details->setText(
          QCoreApplication::translate(
              "FileDropBridge", "Check network connection, receiver port, and localsend-cli.exe; see logs for details."
          )
      );
    }
    show();
    m_hide.start(success ? 8000 : 20000);
  }

private:
  bool m_receiving;
  QLabel *m_title;
  QLabel *m_details;
  QProgressBar *m_bar;
  QElapsedTimer m_elapsed;
  QTimer m_hide;
  quint64 m_done = 0;
  quint64 m_total = 0;
};

TransferStatus &transferStatus(bool receiving)
{
  static auto *sender = new TransferStatus(false);
  static auto *receiver = new TransferStatus(true);
  return receiving ? *receiver : *sender;
}

void transferBegin(bool receiving, const QString &description)
{
  auto *app = QCoreApplication::instance();
  if (!app || !app->inherits("QApplication")) {
    return;
  }
  if (QThread::currentThread() != app->thread()) {
    QMetaObject::invokeMethod(
        app, [receiving, description]() { transferStatus(receiving).begin(description); }, Qt::QueuedConnection
    );
    return;
  }
  transferStatus(receiving).begin(description);
}

void transferProgress(bool receiving, quint64 done, quint64 total)
{
  auto *app = QCoreApplication::instance();
  if (!app || !app->inherits("QApplication")) {
    return;
  }
  if (QThread::currentThread() != app->thread()) {
    QMetaObject::invokeMethod(
        app, [receiving, done, total]() { transferStatus(receiving).progress(done, total); }, Qt::QueuedConnection
    );
    return;
  }
  transferStatus(receiving).progress(done, total);
}

void transferFinish(bool receiving, bool success, const QString &message)
{
  auto *app = QCoreApplication::instance();
  if (!app || !app->inherits("QApplication")) {
    return;
  }
  if (QThread::currentThread() != app->thread()) {
    QMetaObject::invokeMethod(
        app, [receiving, success, message]() { transferStatus(receiving).finish(success, message); },
        Qt::QueuedConnection
    );
    return;
  }
  transferStatus(receiving).finish(success, message);
}

uint64_t nowMs()
{
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string generateAuthToken()
{
  std::array<quint32, 8> words{};
  QRandomGenerator::system()->fillRange(words.data(), static_cast<qsizetype>(words.size()));
  const QByteArray raw(reinterpret_cast<const char *>(words.data()), static_cast<qsizetype>(words.size() * sizeof(quint32)));
  return raw.toHex().toStdString();
}

bool hasTransferableFiles(const std::vector<std::string> &paths)
{
  for (const auto &p : paths) {
    const QFileInfo info(QString::fromStdString(p));
    if (info.isFile()) {
      return true;
    }
    if (info.isDir()) {
      QDirIterator it(info.absoluteFilePath(), QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
      if (it.hasNext()) {
        return true;
      }
    }
  }
  return false;
}

QString resolveLocalSendConfigDir(const char *envOverride, const QString &roleSubdir)
{
  if (const char *cfg = std::getenv(envOverride); cfg && cfg[0] != '\0') {
    return QString::fromUtf8(cfg);
  }
  QString baseDir;
  if (Settings::isPortableMode()) {
    baseDir = Settings::settingsPath();
  } else {
    baseDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (baseDir.isEmpty()) {
      baseDir = Settings::settingsPath();
    }
  }
  if (baseDir.isEmpty()) {
    baseDir = QDir::tempPath();
  }
  return QDir(baseDir).filePath(QStringLiteral("localsend/%1").arg(roleSubdir));
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

void sendRelativeMouseMove(int dx, int dy)
{
  INPUT input = {};
  input.type = INPUT_MOUSE;
  input.mi.dx = static_cast<LONG>(dx);
  input.mi.dy = static_cast<LONG>(dy);
  input.mi.dwFlags = MOUSEEVENTF_MOVE;
  SendInput(1, &input, sizeof(input));
}

void sendLeftButtonUp(ULONG_PTR extraInfo)
{
  INPUT input = {};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = MOUSEEVENTF_LEFTUP;
  input.mi.dwExtraInfo = extraInfo;
  SendInput(1, &input, sizeof(input));
}

void sendEscapeKeyUp(ULONG_PTR extraInfo)
{
  INPUT input = {};
  input.type = INPUT_KEYBOARD;
  input.ki.wVk = VK_ESCAPE;
  input.ki.dwFlags = KEYEVENTF_KEYUP;
  input.ki.dwExtraInfo = extraInfo;
  SendInput(1, &input, sizeof(input));
}

std::string pidlToUtf8Path(PCIDLIST_ABSOLUTE pidl)
{
  if (!pidl) {
    return {};
  }
  std::vector<wchar_t> buf(32768, L'\0');
  if (!SHGetPathFromIDListEx(pidl, buf.data(), static_cast<DWORD>(buf.size()), GPFIDL_DEFAULT)) {
    return {};
  }
  return wideToUtf8(std::wstring(buf.data()));
}

HWND findActiveExplorerTab(HWND hwndTop)
{
  HWND tab = nullptr;
  while ((tab = FindWindowExW(hwndTop, tab, L"ShellTabWindowClass", nullptr)) != nullptr) {
    if (IsWindowVisible(tab)) {
      return tab;
    }
  }
  return nullptr;
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
        UINT charsNeeded = DragQueryFileW(drop, i, nullptr, 0);
        if (charsNeeded > 0) {
          std::wstring path(static_cast<size_t>(charsNeeded) + 1, L'\0');
          UINT copied = DragQueryFileW(drop, i, path.data(), static_cast<UINT>(path.size()));
          if (copied > 0) {
            path.resize(copied);
            results.push_back(wideToUtf8(path));
          }
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
          std::string resolved = pidlToUtf8Path(full);
          if (!resolved.empty()) {
            results.push_back(std::move(resolved));
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

void cancelNativeDragBeforeSwitch(int sideIndex);
void armNativeDragHandoff(int sideIndex);

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
      if (FileDropBridge::instance().setPendingDragFiles(std::move(files))) {
        armNativeDragHandoff(sideIndex);
      }
      // 本地目标永远不接收原生放下，避免源文件或桌面图标被移动。
      *eff = DROPEFFECT_NONE;
    } else {
      *eff = DROPEFFECT_NONE;
    }
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *eff) override
  {
    *eff = DROPEFFECT_NONE;
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
    (void)obj;
    (void)pt;
    *eff = DROPEFFECT_NONE;
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
  std::atomic<bool> cancellingNativeDrag{false};
  std::atomic<bool> nativeDragCancellationComplete{false};
  std::atomic<int> cancellingSide{0};
  std::atomic<HWND> cancelTimerWindow{nullptr};

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
    if (cancellingNativeDrag.exchange(false)) {
      if (HWND timerHwnd = cancelTimerWindow.exchange(nullptr)) {
        KillTimer(timerHwnd, 1);
      }
      sendEscapeKeyUp(static_cast<ULONG_PTR>(0x44534B54));
    }
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

void nudgeAcrossEdge(int side)
{
  const int dx = side == 0 ? -8 : side == 1 ? 8 : 0;
  const int dy = side == 2 ? -8 : side == 3 ? 8 : 0;
  sendRelativeMouseMove(dx, dy);
}

void armNativeDragHandoff(int sideIndex)
{
  g_sentinelMgr.cancellingSide.store(sideIndex);
  g_sentinelMgr.nativeDragCancellationComplete.store(false);
  nudgeAcrossEdge(sideIndex);
}

void CALLBACK finishNativeDragCancellation(HWND hwnd, UINT, UINT_PTR timerId, DWORD)
{
  KillTimer(hwnd, timerId);
  g_sentinelMgr.cancelTimerWindow.store(nullptr);
  if (!g_sentinelMgr.cancellingNativeDrag.exchange(false)) {
    return;
  }
  sendEscapeKeyUp(static_cast<ULONG_PTR>(0x44534B54));
  g_sentinelMgr.nativeDragCancellationComplete.store(true);
  if (!FileDropBridge::instance().hasPendingDragFiles()) {
    return;
  }
  if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
    FileDropBridge::instance().cancelActiveDrag();
    return;
  }
  LOG_INFO("FileDropBridge: native drag cancellation completed before screen handoff");
}

void cancelNativeDragBeforeSwitch(int sideIndex)
{
  if (sideIndex < 0 || sideIndex >= 4) {
    FileDropBridge::instance().cancelActiveDrag();
    return;
  }
  // 保留源窗口的前台状态，让 OLE 消费 Esc 后再交出鼠标，避免中央残影。
  const HWND hwnd = g_sentinelMgr.hwnds.at(static_cast<size_t>(sideIndex));
  g_sentinelMgr.cancellingNativeDrag.store(true);
  g_sentinelMgr.cancellingSide.store(sideIndex);
  g_sentinelMgr.cancelTimerWindow.store(hwnd);
  if (!SetTimer(hwnd, 1, 80, finishNativeDragCancellation)) {
    g_sentinelMgr.cancellingNativeDrag.store(false);
    g_sentinelMgr.cancelTimerWindow.store(nullptr);
    FileDropBridge::instance().cancelActiveDrag();
    LOG_WARN("FileDropBridge: could not schedule native drag cancellation");
    return;
  }
  INPUT escape = {};
  escape.type = INPUT_KEYBOARD;
  escape.ki.wVk = VK_ESCAPE;
  escape.ki.dwExtraInfo = static_cast<ULONG_PTR>(0x44534B54);
  if (SendInput(1, &escape, sizeof(escape)) != 1) {
    KillTimer(hwnd, 1);
    g_sentinelMgr.cancellingNativeDrag.store(false);
    g_sentinelMgr.cancelTimerWindow.store(nullptr);
    FileDropBridge::instance().cancelActiveDrag();
    LOG_WARN("FileDropBridge: could not cancel source native drag");
  }
}

void assignProcessToKillOnCloseJob(qint64 pid)
{
  if (pid <= 0) {
    return;
  }
  struct KillOnCloseJob
  {
    HANDLE handle = nullptr;
    KillOnCloseJob()
    {
      handle = CreateJobObjectW(nullptr, nullptr);
      if (handle) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(handle, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
      }
    }
    ~KillOnCloseJob()
    {
      if (handle) {
        CloseHandle(handle);
        handle = nullptr;
      }
    }
  };
  static KillOnCloseJob s_job;
  if (!s_job.handle) {
    return;
  }
  HANDLE hProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
  if (hProc) {
    AssignProcessToJobObject(s_job.handle, hProc);
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
  m_canCaptureDrag = false;
  m_sourceButtonNeedsRelease = false;
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

void FileDropBridge::onPrimaryLeftMouseDown()
{
  std::scoped_lock lock(m_mutex);
  m_pendingFiles.clear();
  m_canCaptureDrag = true;
}

void FileDropBridge::onLeftMouseUp(bool onSecondary)
{
  bool releaseSourceButton;
  {
    std::scoped_lock lock(m_mutex);
    releaseSourceButton = m_sourceButtonNeedsRelease;
    m_sourceButtonNeedsRelease = false;
  }
#if defined(_WIN32)
  if (onSecondary && releaseSourceButton) {
    // 物理松手在副屏时被钩子转发，另给源系统补一次松手，清除本地按下状态。
    // 此事件只到本地 OS，不再次转发给副机，也不触发发送逻辑。
    sendLeftButtonUp(static_cast<ULONG_PTR>(0x44534B54));
  }
#else
  (void)onSecondary;
  (void)releaseSourceButton;
#endif
}

bool FileDropBridge::canSwitchFromPrimary()
{
#if defined(_WIN32)
  if (g_sentinelMgr.cancellingNativeDrag.load()) {
    return false;
  }
  // 只有 Server 已允许切屏时才取消本地拖拽，保留锁屏和锁定角落的行为。
  if (hasPendingDragFiles() && !g_sentinelMgr.nativeDragCancellationComplete.load()) {
    cancelNativeDragBeforeSwitch(g_sentinelMgr.cancellingSide.load());
    return false;
  }
#endif
  return true;
}

bool FileDropBridge::setPendingDragFiles(std::vector<std::string> files)
{
  std::scoped_lock lock(m_mutex);
  if (!m_canCaptureDrag || files.empty()) {
    LOG_DEBUG("FileDropBridge: ignoring repeated native drag capture without a new primary button press");
    return false;
  }
  m_canCaptureDrag = false;
  m_sourceButtonNeedsRelease = true;
  m_pendingFiles = std::move(files);
  m_pendingTimestampMs = nowMs();
  return true;
}

bool FileDropBridge::hasPendingDragFiles() const
{
  std::scoped_lock lock(m_mutex);
  if (m_pendingFiles.empty()) {
    return false;
  }
  return (nowMs() - m_pendingTimestampMs) <= 4000;
}

bool FileDropBridge::hasActiveDragFiles() const
{
  std::scoped_lock lock(m_mutex);
  return !m_activeDragFiles.empty();
}

bool FileDropBridge::hasActiveTransfers() const
{
  std::scoped_lock lock(m_mutex);
  return m_sendingInProgress || !m_sendQueue.empty() || m_activeReceiveSession;
}

void FileDropBridge::notifyActiveTransfersChangedLocked()
{
  const bool active = m_sendingInProgress || !m_sendQueue.empty() || m_activeReceiveSession;
  if (active == m_lastReportedActiveTransfers) {
    return;
  }
  m_lastReportedActiveTransfers = active;
  LOG_INFO("FileDropBridge: activeTransfers=%d", active ? 1 : 0);
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
  }

  if (!hasTransferableFiles(files)) {
    LOG_INFO("FileDropBridge: dragged path(s) contain no transferable files; skipping cross-screen drag");
    return;
  }

  std::string token;
  {
    std::scoped_lock lock(m_mutex);
    m_activeDragFiles = files;
    m_dragTargetClient = dstClient;
    if (dstClient) {
      token = generateAuthToken();
    }
    m_activeDragToken = token;
  }

  if (dstClient) {
    std::string msg = "DESKSEND_START:" + token;
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
  std::string oldToken;
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeDragFiles.empty()) {
      return;
    }
    LOG_INFO("FileDropBridge: cursor returned to primary before release; cancelling file drag");
    m_pendingFiles.clear();
    m_canCaptureDrag = false;
    m_activeDragFiles.clear();
    oldTarget = m_dragTargetClient;
    oldToken = std::move(m_activeDragToken);
    m_dragTargetClient = nullptr;
    m_activeDragToken.clear();
  }
  if (oldTarget) {
    const std::string cancelMsg = oldToken.empty() ? std::string("DESKSEND_CANCEL") : ("DESKSEND_CANCEL:" + oldToken);
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
    m_activeDragToken.clear();
  }
}

void FileDropBridge::cancelActiveDrag()
{
  BaseClientProxy *oldTarget = nullptr;
  std::string oldToken;
  {
    std::scoped_lock lock(m_mutex);
    LOG_INFO("FileDropBridge: Escape pressed; cancelling file drag");
    m_pendingFiles.clear();
    m_canCaptureDrag = false;
    m_activeDragFiles.clear();
    oldTarget = m_dragTargetClient;
    oldToken = std::move(m_activeDragToken);
    m_dragTargetClient = nullptr;
    m_activeDragToken.clear();
  }
  if (oldTarget) {
    const std::string cancelMsg = oldToken.empty() ? std::string("DESKSEND_CANCEL") : ("DESKSEND_CANCEL:" + oldToken);
    oldTarget->sendDragInfo(0, cancelMsg.data(), cancelMsg.size());
  }
}

void FileDropBridge::onSecondaryLeftMouseUp(BaseClientProxy *activeClient, int32_t x, int32_t y)
{
  std::vector<std::string> files;
  std::string token;
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeDragFiles.empty() || activeClient == nullptr) {
      return;
    }
    files = std::move(m_activeDragFiles);
    token = std::move(m_activeDragToken);
    m_activeDragFiles.clear();
    m_activeDragToken.clear();
    m_dragTargetClient = nullptr;
  }

  auto sendCancel = [&]() {
    const std::string cancelMsg = token.empty() ? std::string("DESKSEND_CANCEL") : ("DESKSEND_CANCEL:" + token);
    activeClient->sendDragInfo(0, cancelMsg.data(), cancelMsg.size());
  };

  if (!hasTransferableFiles(files)) {
    LOG_WARN("FileDropBridge: dragged path(s) contain no transferable files; cancelling drop");
    sendCancel();
    return;
  }

  std::string peerIp = getClientPeerIp(activeClient);
  if (peerIp.empty()) {
    LOG_WARN("FileDropBridge: could not determine peer IP for client \"%s\"", activeClient->getName().c_str());
    sendCancel();
    return;
  }

  std::string dropMsg = "DESKSEND_DROP:" + std::to_string(x) + "," + std::to_string(y);
  const std::string tag = computeTokenTag(token);
  if (!tag.empty()) {
    dropMsg += ":" + tag;
  }
  activeClient->sendDragInfo(static_cast<uint32_t>(files.size()), dropMsg.data(), dropMsg.size());

  LOG_INFO(
      "FileDropBridge: drop at (%d,%d) on \"%s\" (%s), sending %zu file(s) via LocalSend", x, y,
      activeClient->getName().c_str(), peerIp.c_str(), files.size()
  );
  sendFilesViaLocalSend(peerIp, files, token);
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

std::string FileDropBridge::computeTokenTag(const std::string &token)
{
  if (token.empty()) {
    return {};
  }
  const QByteArray digest = QCryptographicHash::hash(
      QByteArray::fromRawData(token.data(), static_cast<qsizetype>(token.size())), QCryptographicHash::Sha256
  );
  return digest.toHex().left(16).toStdString();
}

void FileDropBridge::sendFilesViaLocalSend(
    const std::string &targetIp, const std::vector<std::string> &files, const std::string &authToken
)
{
  if (auto *app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
    QMetaObject::invokeMethod(
        app, [this, targetIp, files, authToken]() { sendFilesViaLocalSend(targetIp, files, authToken); },
        Qt::QueuedConnection
    );
    return;
  }

  {
    std::scoped_lock lock(m_mutex);
    m_sendQueue.push_back(SendJob{targetIp, files, authToken});
    const bool alreadySending = m_sendingInProgress;
    if (!alreadySending) {
      m_sendingInProgress = true;
      notifyActiveTransfersChangedLocked();
    } else {
      notifyActiveTransfersChangedLocked();
      LOG_INFO("FileDropBridge: queued send job to %s (%zu file(s)); %zu job(s) waiting", targetIp.c_str(), files.size(), m_sendQueue.size());
      return;
    }
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
      notifyActiveTransfersChangedLocked();
      return;
    }
    job = std::move(m_sendQueue.front());
    m_sendQueue.pop_front();
    notifyActiveTransfersChangedLocked();
  }

  if (job.authToken.empty()) {
    LOG_WARN("FileDropBridge: missing authorization token for send job to %s; aborting", job.targetIp.c_str());
    transferFinish(false, false, QCoreApplication::translate("FileDropBridge", "Send failed: missing transfer authorization token"));
    processNextSendJob();
    return;
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
  args << QStringLiteral("send") << QStringLiteral("--to") << destination << QStringLiteral("--pin-stdin")
       << QStringLiteral("--paths-stdin");

  QByteArray stdinPayload = QByteArray::fromStdString(job.authToken + "\n");
  for (const auto &f : job.files) {
    QByteArray line = QByteArray::fromStdString(f);
    line.replace('\r', "");
    line.replace('\n', "");
    if (!line.isEmpty()) {
      stdinPayload.append(line);
      stdinPayload.append('\n');
    }
  }

  auto *proc = new QProcess();
  proc->setProcessChannelMode(QProcess::MergedChannels);
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  const QString senderCfgDir = resolveLocalSendConfigDir("DESKSEND_SENDER_CONFIG_HOME", QStringLiteral("sender"));
  QDir().mkpath(senderCfgDir);
  env.insert(QStringLiteral("XDG_CONFIG_HOME"), senderCfgDir);
  proc->setProcessEnvironment(env);
  transferBegin(false, QCoreApplication::translate("FileDropBridge", "Sending to %1").arg(destination));
  QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, [proc]() {
    while (proc->canReadLine()) {
      const QString line = QString::fromUtf8(proc->readLine()).trimmed();
      if (line.startsWith(QStringLiteral("SEND_PROGRESS:"))) {
        const auto fields = line.split(':');
        if (fields.size() == 3) {
          transferProgress(false, fields[1].toULongLong(), fields[2].toULongLong());
        }
      } else {
        LOG_INFO("FileDropBridge [tx]: %s", line.toStdString().c_str());
      }
    }
  });

  QObject::connect(proc, &QProcess::started, proc, [proc, stdinPayload]() {
#if defined(_WIN32)
    assignProcessToKillOnCloseJob(proc->processId());
#endif
    if (proc->write(stdinPayload) == stdinPayload.size()) {
      proc->waitForBytesWritten(2000);
    }
    proc->closeWriteChannel();
  });

  QObject::connect(proc, &QProcess::errorOccurred, proc, [this, proc, destination](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      LOG_WARN("FileDropBridge: failed to start localsend-cli for %s", destination.toStdString().c_str());
      transferFinish(false, false, QCoreApplication::translate("FileDropBridge", "Send failed: unable to launch localsend-cli.exe"));
      proc->deleteLater();
      processNextSendJob();
    }
  });

  QObject::connect(proc, &QProcess::finished, proc, [this, proc, destination](int exitCode, QProcess::ExitStatus status) {
    const QString out = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
    const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed();
    const bool success = exitCode == 0 && status == QProcess::NormalExit;
    transferFinish(
        false, success,
        success ? QCoreApplication::translate("FileDropBridge", "File transfer completed")
                : QCoreApplication::translate("FileDropBridge", "File transfer failed")
    );
    if (success) {
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
  startReceiverInternal(serverHost, false);
}

void FileDropBridge::startReceiverInternal(const std::string &serverHost, bool isRestart)
{
  QProcess *oldProc = nullptr;
  QString staging;
  uint32_t generation = 0;
  {
    std::scoped_lock lock(m_mutex);
    if (isRestart && (!m_receiverDesired || m_currentAllowedServerIp != serverHost)) {
      return;
    }
    if (!isRestart && m_receiverProcess && m_receiverProcess->state() != QProcess::NotRunning &&
        m_currentAllowedServerIp == serverHost && m_receiverDesired) {
      return;
    }
    oldProc = m_receiverProcess;
    m_receiverProcess = nullptr;
    ++m_receiverGeneration;
    generation = m_receiverGeneration;
    m_receiverDesired = true;
    m_currentAllowedServerIp = serverHost;
    if (!isRestart) {
      m_receiverRestartAttempt = 0;
      m_pendingReceiverAuthLines.clear();
      m_pendingDropTargets.clear();
      m_activeSessionTarget = DropTargetEntry{};
      m_lastResolvedDropDir.clear();
      m_activeReceiveSession = false;
      notifyActiveTransfersChangedLocked();
    }
    staging = QDir(QDir::tempPath()).filePath(QStringLiteral("DeskSend_Staging"));
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
    m_defaultDropFallbackDir = defaultDrop.toStdString();
  }

  if (oldProc) {
    oldProc->disconnect();
    oldProc->kill();
    oldProc->waitForFinished(1000);
    delete oldProc;
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
       << QStringLiteral("--auto-accept-from") << allowSpec << QStringLiteral("--auth-stdin");

  auto *proc = new QProcess();
  proc->setProcessChannelMode(QProcess::MergedChannels);
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  const QString rxCfgDir = resolveLocalSendConfigDir("DESKSEND_RECEIVER_CONFIG_HOME", QStringLiteral("receiver"));
  QDir().mkpath(rxCfgDir);
  env.insert(QStringLiteral("XDG_CONFIG_HOME"), rxCfgDir);
  proc->setProcessEnvironment(env);

  {
    std::scoped_lock lock(m_mutex);
    if (generation != m_receiverGeneration || !m_receiverDesired) {
      delete proc;
      return;
    }
    m_receiverProcess = proc;
  }

  QObject::connect(proc, &QProcess::started, proc, [this, proc, generation]() {
    std::deque<std::string> pending;
    {
      std::scoped_lock lock(m_mutex);
      if (generation != m_receiverGeneration || m_receiverProcess != proc) {
        return;
      }
      pending.swap(m_pendingReceiverAuthLines);
    }
#if defined(_WIN32)
    assignProcessToKillOnCloseJob(proc->processId());
#endif
    for (const auto &cmd : pending) {
      const QByteArray line = QByteArray::fromStdString(cmd + "\n");
      proc->write(line);
    }
  });

  QObject::connect(proc, &QProcess::errorOccurred, proc, [this, proc, generation](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      bool shouldWarn = false;
      {
        std::scoped_lock lock(m_mutex);
        if (m_receiverProcess == proc) {
          m_receiverProcess = nullptr;
        }
        shouldWarn = (generation == m_receiverGeneration);
      }
      proc->deleteLater();
      if (shouldWarn) {
        transferFinish(
            true, false,
            QCoreApplication::translate("FileDropBridge", "Receiver failed to start: unable to launch localsend-cli.exe")
        );
      }
    }
  });

  QObject::connect(
      proc, &QProcess::finished, proc, [this, proc, generation](int exitCode, QProcess::ExitStatus exitStatus) {
        proc->deleteLater();
        std::string restartHost;
        int delayMs = 0;
        int attempt = 0;
        bool shouldRestart = false;
        bool hadActiveSession = false;
        bool currentGen = false;
        {
          std::scoped_lock lock(m_mutex);
          if (m_receiverProcess == proc) {
            m_receiverProcess = nullptr;
          }
          currentGen = (generation == m_receiverGeneration);
          if (currentGen) {
            hadActiveSession = m_activeReceiveSession;
            if (m_receiverDesired && !m_currentAllowedServerIp.empty()) {
              shouldRestart = true;
              restartHost = m_currentAllowedServerIp;
              const int shift = std::min(m_receiverRestartAttempt, 4);
              delayMs = std::min(15000, 1000 * (1 << shift));
              ++m_receiverRestartAttempt;
              attempt = m_receiverRestartAttempt;
            }
          }
        }
        if (!currentGen) {
          return;
        }
        if (hadActiveSession) {
          handleReceiveSessionFinished(true);
        }
        transferFinish(
            true, false,
            shouldRestart
                ? QCoreApplication::translate(
                      "FileDropBridge", "File receiver stopped unexpectedly, attempting automatic recovery..."
                  )
                : QCoreApplication::translate("FileDropBridge", "File receiver stopped; verify that the port is available")
        );
        if (shouldRestart) {
          LOG_WARN(
              "FileDropBridge: receiver exited unexpectedly (code=%d, status=%d); restarting in %d ms (attempt %d)",
              exitCode, static_cast<int>(exitStatus), delayMs, attempt
          );
          if (auto *app = QCoreApplication::instance()) {
            QTimer::singleShot(delayMs, app, [this, generation, restartHost]() {
              {
                std::scoped_lock lock(m_mutex);
                if (!m_receiverDesired || generation != m_receiverGeneration ||
                    m_currentAllowedServerIp != restartHost) {
                  return;
                }
              }
              startReceiverInternal(restartHost, true);
            });
          }
        }
      }
  );

  QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, [this, proc, generation]() {
    while (proc->canReadLine()) {
      const QString line = QString::fromUtf8(proc->readLine()).trimmed();
      if (!line.startsWith(QStringLiteral("RECEIVE_PROGRESS:"))) {
        LOG_INFO("FileDropBridge [rx]: %s", line.toStdString().c_str());
      }
      {
        std::scoped_lock lock(m_mutex);
        if (generation != m_receiverGeneration) {
          return;
        }
        if (line.startsWith(QStringLiteral("LOCALSEND_RECEIVER_READY:"))) {
          m_receiverRestartAttempt = 0;
        }
      }
      const QString filePrefix = QStringLiteral("RECEIVED_FILE:");
      const QString startPrefix = QStringLiteral("RECEIVE_SESSION_STARTED:");
      if (line.startsWith(QStringLiteral("RECEIVE_PROGRESS:"))) {
        const auto fields = line.split(':');
        if (fields.size() == 4) {
          transferProgress(true, fields[2].toULongLong(), fields[3].toULongLong());
        }
      } else if (line.startsWith(filePrefix)) {
        handleReceivedFileLine(line.mid(filePrefix.size()).toStdString());
      } else if (line.startsWith(QStringLiteral("RECEIVE_FILE_FAILED:"))) {
        handleReceiveFileFailedLine();
      } else if (line.startsWith(startPrefix)) {
        QString header = line;
        std::string tokenTag;
        const int tokSep = header.lastIndexOf(QStringLiteral(":tok="));
        if (tokSep > 0) {
          tokenTag = header.mid(tokSep + 5).toStdString();
          header = header.left(tokSep);
        }
        size_t expectedCount = 0;
        const int lastColon = header.lastIndexOf(':');
        if (lastColon > 0) {
          expectedCount = static_cast<size_t>(header.mid(lastColon + 1).toUInt());
        }
        handleReceiveSessionStarted(expectedCount, tokenTag);
      } else if (line.startsWith(QStringLiteral("RECEIVE_SESSION_FINISHED:"))) {
        handleReceiveSessionFinished(false);
      } else if (line.startsWith(QStringLiteral("RECEIVE_SESSION_CANCELLED:"))) {
        handleReceiveSessionFinished(true);
      }
    }
  });

  LOG_INFO(
      "FileDropBridge: starting headless receiver: %s %s", cliPath.toStdString().c_str(),
      args.join(' ').toStdString().c_str()
  );
  proc->start(cliPath, args);
}

void FileDropBridge::stopReceiver()
{
  if (auto *app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
    QMetaObject::invokeMethod(app, [this]() { stopReceiver(); }, Qt::QueuedConnection);
    return;
  }

  QProcess *oldProc = nullptr;
  {
    std::scoped_lock lock(m_mutex);
    m_receiverDesired = false;
    ++m_receiverGeneration;
    m_receiverRestartAttempt = 0;
    m_currentAllowedServerIp.clear();
    m_pendingReceiverAuthLines.clear();
    m_pendingDropTargets.clear();
    m_activeSessionTarget = DropTargetEntry{};
    m_activeSessionPlacedFiles.clear();
    m_activeSessionExpectedFiles = 0;
    m_activeSessionHandledFiles = 0;
    m_activeSessionEnded = false;
    m_activeSessionFailed = false;
    m_activeReceiveSession = false;
    m_lastResolvedDropDir.clear();
    notifyActiveTransfersChangedLocked();
    oldProc = m_receiverProcess;
    m_receiverProcess = nullptr;
  }
  if (oldProc) {
    oldProc->disconnect();
    oldProc->kill();
    oldProc->waitForFinished(1000);
    delete oldProc;
  }
}

void FileDropBridge::queueReceiverAuthCommand(const std::string &command)
{
  if (auto *app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
    QMetaObject::invokeMethod(
        app, [this, command]() { queueReceiverAuthCommand(command); }, Qt::QueuedConnection
    );
    return;
  }

  std::scoped_lock lock(m_mutex);
  if (!m_receiverDesired) {
    return;
  }
  if (m_receiverProcess && m_receiverProcess->state() == QProcess::Running) {
    const QByteArray line = QByteArray::fromStdString(command + "\n");
    m_receiverProcess->write(line);
  } else {
    if (m_pendingReceiverAuthLines.size() >= 64) {
      m_pendingReceiverAuthLines.pop_front();
    }
    m_pendingReceiverAuthLines.push_back(command);
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
      const std::string rest = coords.substr(comma + 1);
      const auto tagSep = rest.find(':');
      const int32_t y = std::atoi((tagSep == std::string::npos ? rest : rest.substr(0, tagSep)).c_str());
      std::string tokenTag = (tagSep == std::string::npos) ? std::string{} : rest.substr(tagSep + 1);
      const size_t nl = tokenTag.find_first_of("\r\n");
      if (nl != std::string::npos) {
        tokenTag.resize(nl);
      }
      std::string fallback;
      {
        std::scoped_lock lock(m_mutex);
        fallback = !m_lastResolvedDropDir.empty() ? m_lastResolvedDropDir : m_defaultDropFallbackDir;
      }
      bool isDirectFolder = true;
      std::string resolved = resolveDropTargetDirectory(x, y, fallback, &isDirectFolder);
      {
        std::scoped_lock lock(m_mutex);
        const uint64_t now = nowMs();
        while (!m_pendingDropTargets.empty() && now >= m_pendingDropTargets.front().timestampMs &&
               (now - m_pendingDropTargets.front().timestampMs > 300000)) {
          m_pendingDropTargets.pop_front();
        }
        m_lastResolvedDropDir = resolved;
        m_pendingDropTargets.push_back(DropTargetEntry{resolved, !isDirectFolder, now, tokenTag});
      }
      LOG_INFO(
          "FileDropBridge: remote drop at (%d,%d) for %u file(s) -> target folder \"%s\" (directFolder=%d)", x, y,
          fileCount, resolved.c_str(), isDirectFolder ? 1 : 0
      );
    }
  } else if (info.rfind("DESKSEND_START", 0) == 0) {
    const std::string startPrefix = "DESKSEND_START:";
    if (info.rfind(startPrefix, 0) == 0) {
      const size_t newlinePos = info.find('\n', startPrefix.size());
      std::string token = (newlinePos == std::string::npos)
                              ? info.substr(startPrefix.size())
                              : info.substr(startPrefix.size(), newlinePos - startPrefix.size());
      if (!token.empty() && token.back() == '\r') {
        token.pop_back();
      }
      if (!token.empty() && token.find_first_of(" \t\r\n") == std::string::npos) {
        queueReceiverAuthCommand("GRANT " + token);
      }
    }
    LOG_INFO("FileDropBridge: incoming drag started (%u file(s))", fileCount);
  } else if (info.rfind("DESKSEND_CANCEL", 0) == 0) {
    const std::string cancelPrefix = "DESKSEND_CANCEL:";
    if (info.rfind(cancelPrefix, 0) == 0) {
      std::string token = info.substr(cancelPrefix.size());
      const size_t endPos = token.find_first_of("\r\n");
      if (endPos != std::string::npos) {
        token.resize(endPos);
      }
      if (!token.empty() && token.find_first_of(" \t\r\n") == std::string::npos) {
        queueReceiverAuthCommand("REVOKE " + token);
        const std::string cancelTag = computeTokenTag(token);
        if (!cancelTag.empty()) {
          std::scoped_lock lock(m_mutex);
          std::erase_if(m_pendingDropTargets, [&cancelTag](const DropTargetEntry &entry) {
            return entry.tokenTag == cancelTag;
          });
        }
      }
    }
    LOG_INFO("FileDropBridge: incoming drag cancelled");
  }
}

void FileDropBridge::handleReceiveSessionStarted(size_t expectedFiles, const std::string &tokenTag)
{
  finalizeActiveSession();
  {
    std::scoped_lock lock(m_mutex);
    const uint64_t now = nowMs();
    while (!m_pendingDropTargets.empty() && now >= m_pendingDropTargets.front().timestampMs &&
           (now - m_pendingDropTargets.front().timestampMs > 300000)) {
      m_pendingDropTargets.pop_front();
    }
    m_activeSessionPlacedFiles.clear();
    m_activeSessionExpectedFiles = expectedFiles;
    m_activeSessionHandledFiles = 0;
    m_activeSessionEnded = false;
    m_activeSessionFailed = false;
    m_activeReceiveSession = true;
    notifyActiveTransfersChangedLocked();
    if (!tokenTag.empty()) {
      auto it = std::find_if(
          m_pendingDropTargets.begin(), m_pendingDropTargets.end(),
          [&tokenTag](const DropTargetEntry &entry) { return entry.tokenTag == tokenTag; }
      );
      if (it == m_pendingDropTargets.end()) {
        // Legacy fallback when the drop message had no token tag
        it = std::find_if(
            m_pendingDropTargets.begin(), m_pendingDropTargets.end(),
            [](const DropTargetEntry &entry) { return entry.tokenTag.empty(); }
        );
      }
      if (it != m_pendingDropTargets.end()) {
        m_activeSessionTarget = std::move(*it);
        m_pendingDropTargets.erase(m_pendingDropTargets.begin(), std::next(it));
      } else {
        m_activeSessionTarget = DropTargetEntry{};
        LOG_WARN("FileDropBridge: no matching pending drop target for token tag; files will remain in staging");
      }
    } else if (!m_pendingDropTargets.empty()) {
      m_activeSessionTarget = std::move(m_pendingDropTargets.front());
      m_pendingDropTargets.pop_front();
    } else {
      m_activeSessionTarget = DropTargetEntry{};
      LOG_WARN("FileDropBridge: no pending drop target for receive session; files will remain in staging");
    }
  }
  transferBegin(
      true,
      QCoreApplication::translate("FileDropBridge", "Receiving %1 file(s)").arg(static_cast<qulonglong>(expectedFiles))
  );
}

void FileDropBridge::handleReceiveSessionFinished(bool cancelled)
{
  bool readyToFinalize = false;
  {
    std::scoped_lock lock(m_mutex);
    m_activeSessionEnded = true;
    m_activeSessionFailed = m_activeSessionFailed || cancelled;
    if (cancelled || m_activeSessionExpectedFiles == 0 ||
        m_activeSessionHandledFiles >= m_activeSessionExpectedFiles) {
      readyToFinalize = true;
    }
  }
  if (readyToFinalize) {
    finalizeActiveSession();
  }
}

void FileDropBridge::handleReceiveFileFailedLine()
{
  bool readyToFinalize = false;
  {
    std::scoped_lock lock(m_mutex);
    ++m_activeSessionHandledFiles;
    m_activeSessionFailed = true;
    if (m_activeSessionEnded && m_activeSessionHandledFiles >= m_activeSessionExpectedFiles) {
      readyToFinalize = true;
    }
  }
  if (readyToFinalize) {
    finalizeActiveSession();
  }
}

void FileDropBridge::finalizeActiveSession()
{
  std::vector<std::string> filesForClipboard;
  bool shouldCopyToClipboard = false;
  bool sessionEnded = false;
  bool sessionFailed = false;
  std::string stagingDirStr;
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeSessionTarget.dir.empty() && m_activeSessionPlacedFiles.empty() && !m_activeSessionEnded) {
      return;
    }
    shouldCopyToClipboard = m_activeSessionTarget.copyToClipboard;
    sessionEnded = m_activeSessionEnded;
    sessionFailed = m_activeSessionFailed;
    if (shouldCopyToClipboard) {
      filesForClipboard = std::move(m_activeSessionPlacedFiles);
    }
    m_activeSessionPlacedFiles.clear();
    m_activeSessionTarget = DropTargetEntry{};
    m_activeSessionExpectedFiles = 0;
    m_activeSessionHandledFiles = 0;
    m_activeSessionEnded = false;
    m_activeSessionFailed = false;
    m_activeReceiveSession = false;
    notifyActiveTransfersChangedLocked();
    stagingDirStr = m_stagingDir;
  }

  if (sessionEnded) {
    transferFinish(
        true, !sessionFailed,
        sessionFailed ? QCoreApplication::translate("FileDropBridge", "File receive failed or transfer was cancelled")
                      : QCoreApplication::translate("FileDropBridge", "File receive completed")
    );
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

std::string FileDropBridge::uniqueDestinationPath(const std::string &dstDir, const std::string &relPath)
{
  QDir dstRootDir(QString::fromStdString(dstDir));
  if (!dstRootDir.exists()) {
    dstRootDir.mkpath(QStringLiteral("."));
  }

  QString dstPath = dstRootDir.filePath(QString::fromStdString(relPath));
  QFileInfo dstInfo(dstPath);
  QDir dstParentDir = dstInfo.dir();
  if (!dstParentDir.exists()) {
    dstParentDir.mkpath(QStringLiteral("."));
  }

  const QString fileName = dstInfo.fileName();
  const qsizetype lastDot = fileName.lastIndexOf(QLatin1Char('.'));
  const bool hasExtension = lastDot > 0 && (lastDot + 1) < fileName.size();
  const QString baseName = hasExtension ? fileName.left(lastDot) : fileName;
  const QString suffix = hasExtension ? fileName.mid(lastDot + 1) : QString();
  int counter = 1;
  while (QFileInfo::exists(dstPath)) {
    if (suffix.isEmpty()) {
      dstPath = dstParentDir.filePath(QStringLiteral("%1 (%2)").arg(baseName).arg(counter));
    } else {
      dstPath = dstParentDir.filePath(QStringLiteral("%1 (%2).%3").arg(baseName).arg(counter).arg(suffix));
    }
    ++counter;
  }
  return dstPath.toStdString();
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
    }
    stagingDirStr = m_stagingDir;
  }
  if (targetDir.empty()) {
    LOG_WARN("FileDropBridge: no authorized drop target for received file; keeping in staging");
    return;
  }

  QFileInfo srcInfo(QString::fromStdString(filePath));
  if (!srcInfo.exists()) {
    return;
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

  const QString dstPath = QString::fromStdString(uniqueDestinationPath(targetDir, relPath.toStdString()));

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

  bool readyToFinalize = false;
  {
    std::scoped_lock lock(m_mutex);
    if (placed) {
      m_activeSessionPlacedFiles.push_back(dstPath.toStdString());
    } else {
      m_activeSessionFailed = true;
    }
    ++m_activeSessionHandledFiles;
    if (m_activeSessionEnded && m_activeSessionExpectedFiles > 0 &&
        m_activeSessionHandledFiles >= m_activeSessionExpectedFiles) {
      readyToFinalize = true;
    }
  }
  if (readyToFinalize) {
    finalizeActiveSession();
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
      const HWND activeTab = findActiveExplorerTab(hwndTop);
      std::string matchedFolder;
      std::string fallbackTabFolder;

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
                  const bool isExactHitTab = tabHwnd && (tabHwnd == hwndHit || IsChild(tabHwnd, hwndHit));
                  const bool isActiveTopTab = activeTab ? (tabHwnd == activeTab) : (tabHwnd == nullptr);
                  if (isExactHitTab || (fallbackTabFolder.empty() && isActiveTopTab)) {
                    IShellView *shellView = nullptr;
                    if (SUCCEEDED(shellBrowser->QueryActiveShellView(&shellView)) && shellView) {
                      IFolderView *folderView = nullptr;
                      if (SUCCEEDED(shellView->QueryInterface(IID_PPV_ARGS(&folderView))) && folderView) {
                        IPersistFolder2 *persistFolder = nullptr;
                        if (SUCCEEDED(folderView->GetFolder(IID_PPV_ARGS(&persistFolder))) && persistFolder) {
                          PIDLIST_ABSOLUTE pidl = nullptr;
                          if (SUCCEEDED(persistFolder->GetCurFolder(&pidl)) && pidl) {
                            std::string resolved = pidlToUtf8Path(pidl);
                            if (!resolved.empty()) {
                              if (isExactHitTab) {
                                matchedFolder = std::move(resolved);
                              } else {
                                fallbackTabFolder = std::move(resolved);
                              }
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
      if (matchedFolder.empty()) {
        matchedFolder = std::move(fallbackTabFolder);
      }
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
