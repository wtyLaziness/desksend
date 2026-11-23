/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 DeskSend Contributors
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "deskflow/DropExecutor.h"
#include "deskflow/PlatformDropSupport.h"

#include "base/DirectionTypes.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "deskflow/DragProtocol.h"
#include "deskflow/ipc/CoreIpc.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

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
#include <userenv.h>
#include <windows.h>
#include <wtsapi32.h>
#endif

namespace deskflow {

namespace {

uint64_t nowMs()
{
  using namespace std::chrono;
  return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

int receiverRestartDelayMs(int attempt)
{
  if (const char *envDelay = std::getenv("DESKSEND_RECEIVER_BACKOFF_MS"); envDelay && envDelay[0] != '\0') {
    bool ok = false;
    const int overrideMs = QString::fromUtf8(envDelay).toInt(&ok);
    if (ok && overrideMs >= 0) {
      return overrideMs;
    }
  }
  const int shift = std::min(attempt, 4);
  return std::min(15000, 1000 * (1 << shift));
}

int sendInactivityTimeoutMs()
{
  if (const char *envTimeout = std::getenv("DESKSEND_SEND_INACTIVITY_TIMEOUT_MS");
      envTimeout && envTimeout[0] != '\0') {
    bool ok = false;
    const int overrideMs = QString::fromUtf8(envTimeout).toInt(&ok);
    if (ok && overrideMs >= 50) {
      return overrideMs;
    }
  }
  return 120000;
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

bool hostsEqualUnordered(std::vector<std::string> a, std::vector<std::string> b)
{
  if (a.size() != b.size()) {
    return false;
  }
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  return a == b;
}

std::string joinHostsKey(const std::vector<std::string> &hosts)
{
  std::vector<std::string> sorted = hosts;
  std::sort(sorted.begin(), sorted.end());
  std::string out;
  for (const auto &h : sorted) {
    if (!out.empty()) {
      out += ',';
    }
    out += h;
  }
  return out;
}

#if defined(_WIN32)

constexpr ULONG_PTR kSyntheticMouseExtraInfo = 0x44534B54;

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

std::wstring utf8ToWide(const std::string &s)
{
  if (s.empty()) {
    return {};
  }
  int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
  if (len <= 0) {
    return {};
  }
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
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

  FORMATETC idFmt = {
      static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL
  };
  if (SUCCEEDED(obj->GetData(&idFmt, &med))) {
    auto *cida = static_cast<CIDA *>(GlobalLock(med.hGlobal));
    if (cida) {
      auto parent = reinterpret_cast<PCUIDLIST_ABSOLUTE>(reinterpret_cast<const BYTE *>(cida) + cida->aoffset[0]);
      for (UINT i = 0; i < cida->cidl; ++i) {
        auto child = reinterpret_cast<PCUIDLIST_RELATIVE>(reinterpret_cast<const BYTE *>(cida) + cida->aoffset[i + 1]);
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

HANDLE getSharedKillOnCloseJob()
{
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
  return s_job.handle;
}

void assignProcessHandleToKillOnCloseJob(HANDLE hProc)
{
  HANDLE job = getSharedKillOnCloseJob();
  if (job && hProc) {
    AssignProcessToJobObject(job, hProc);
  }
}

void configureLocalSendChildProcess(qint64 pid)
{
  if (pid <= 0) {
    return;
  }
  const DWORD processId = static_cast<DWORD>(pid);
  if (HANDLE hJobProc = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE, processId)) {
    assignProcessHandleToKillOnCloseJob(hJobProc);
    CloseHandle(hJobProc);
  }
  // 降低传输子进程 CPU 调度优先级，避免磁盘与加密吞吐抢占键鼠转发线程，但不限制传输带宽。
  if (HANDLE hPrioProc = OpenProcess(PROCESS_SET_INFORMATION, FALSE, processId)) {
    SetPriorityClass(hPrioProc, BELOW_NORMAL_PRIORITY_CLASS);
    CloseHandle(hPrioProc);
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
      LOG_INFO(
          "FileDropBridge: copied %zu received file(s) to system clipboard (CF_HDROP) for non-Explorer target",
          utf8Paths.size()
      );
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

bool isLocalSystemToken(HANDLE token)
{
  DWORD needed = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  if (needed == 0) {
    return false;
  }
  std::vector<BYTE> buf(needed);
  if (!GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
    return false;
  }
  auto *user = reinterpret_cast<const TOKEN_USER *>(buf.data());
  return IsWellKnownSid(user->User.Sid, WinLocalSystemSid) != FALSE;
}

bool acquireNonSystemUserPrimaryToken(HANDLE &outPrimaryToken, bool &outUseProcessWithTokenFallback)
{
  outPrimaryToken = nullptr;
  outUseProcessWithTokenFallback = false;

  // Primary path when running as NT AUTHORITY\SYSTEM service: active console session user token.
  const DWORD sessionId = WTSGetActiveConsoleSessionId();
  if (sessionId != 0xFFFFFFFF) {
    HANDLE rawToken = nullptr;
    if (WTSQueryUserToken(sessionId, &rawToken) && rawToken) {
      if (!isLocalSystemToken(rawToken)) {
        HANDLE dup = nullptr;
        const BOOL ok = DuplicateTokenEx(
            rawToken, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &dup
        );
        CloseHandle(rawToken);
        if (ok && dup) {
          outPrimaryToken = dup;
          outUseProcessWithTokenFallback = false;
          return true;
        }
      } else {
        CloseHandle(rawToken);
      }
    }
  }

  // Fallback path when running manually elevated (UAC admin) inside the user session without SE_TCB_NAME.
  HWND shellWnd = GetShellWindow();
  if (!shellWnd) {
    return false;
  }
  DWORD shellPid = 0;
  GetWindowThreadProcessId(shellWnd, &shellPid);
  if (shellPid == 0) {
    return false;
  }
  HANDLE shellProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shellPid);
  if (!shellProc) {
    return false;
  }
  HANDLE shellTok = nullptr;
  const BOOL opened = OpenProcessToken(shellProc, TOKEN_DUPLICATE | TOKEN_QUERY, &shellTok);
  CloseHandle(shellProc);
  if (!opened || !shellTok) {
    return false;
  }
  const bool isSys = isLocalSystemToken(shellTok);
  HANDLE dup = nullptr;
  const BOOL dupOk = !isSys && DuplicateTokenEx(
                                   shellTok, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &dup
                               );
  CloseHandle(shellTok);
  if (!dupOk || !dup) {
    return false;
  }
  outPrimaryToken = dup;
  outUseProcessWithTokenFallback = true;
  return true;
}

#endif // _WIN32

QString buildAllowSpecForHosts(const std::vector<std::string> &hosts)
{
  QStringList tokens;
  for (const auto &host : hosts) {
    QString spec = QString::fromStdString(host);
    if (spec.startsWith(QStringLiteral("::ffff:"), Qt::CaseInsensitive)) {
      spec = spec.mid(7);
    }
#if defined(_WIN32)
    spec = resolveHostAllowSpec(spec);
#else
    if (spec.isEmpty() || spec == QStringLiteral("localhost")) {
      spec = QStringLiteral("127.0.0.1");
    }
#endif
    const QStringList split = spec.split(',', Qt::SkipEmptyParts);
    for (const QString &item : split) {
      if (!tokens.contains(item)) {
        tokens << item;
      }
    }
  }
  return tokens.join(',');
}

QString formatHostPortTarget(const std::string &host, const QString &port)
{
  QString qHost = QString::fromStdString(host).trimmed();
  if (qHost.startsWith(QStringLiteral("::ffff:"), Qt::CaseInsensitive)) {
    qHost = qHost.mid(7);
  }
  if (qHost.contains(':') && !qHost.startsWith('[')) {
    return QStringLiteral("[%1]:%2").arg(qHost, port);
  }
  return QStringLiteral("%1:%2").arg(qHost, port);
}

} // namespace

// --- ReceiverRestartPolicy ---

std::vector<std::string> ReceiverRestartPolicy::normalizeHosts(const std::vector<std::string> &hosts)
{
  std::vector<std::string> out;
  out.reserve(hosts.size());
  for (const auto &raw : hosts) {
    if (raw.empty()) {
      continue;
    }
    QString q = QString::fromStdString(raw);
    if (q.startsWith(QStringLiteral("::ffff:"), Qt::CaseInsensitive)) {
      q = q.mid(7);
    }
    std::string norm = q.toStdString();
    if (norm.empty()) {
      continue;
    }
    if (std::find(out.begin(), out.end(), norm) == out.end()) {
      out.push_back(std::move(norm));
    }
  }
  return out;
}

std::optional<std::vector<std::string>>
ReceiverRestartPolicy::requestHosts(const std::vector<std::string> &hosts, bool dragActive, bool transferActive)
{
  const std::vector<std::string> norm = normalizeHosts(hosts);
  if (!m_hasActive && norm.empty()) {
    m_deferredHosts.clear();
    m_hasDeferred = false;
    return std::nullopt;
  }
  if (m_hasActive && hostsEqualUnordered(norm, m_activeHosts)) {
    m_deferredHosts.clear();
    m_hasDeferred = false;
    return std::nullopt;
  }
  if (dragActive || transferActive) {
    m_deferredHosts = norm;
    m_hasDeferred = true;
    return std::nullopt;
  }
  m_activeHosts = norm;
  m_hasActive = !m_activeHosts.empty();
  m_deferredHosts.clear();
  m_hasDeferred = false;
  return m_activeHosts;
}

std::optional<std::vector<std::string>> ReceiverRestartPolicy::onActivityChanged(bool dragActive, bool transferActive)
{
  if (dragActive || transferActive || !m_hasDeferred) {
    return std::nullopt;
  }
  if (hostsEqualUnordered(m_deferredHosts, m_activeHosts)) {
    m_deferredHosts.clear();
    m_hasDeferred = false;
    return std::nullopt;
  }
  m_activeHosts = std::move(m_deferredHosts);
  m_deferredHosts.clear();
  m_hasDeferred = false;
  m_hasActive = !m_activeHosts.empty();
  return m_activeHosts;
}

bool ReceiverRestartPolicy::hasDeferredRestart() const
{
  return m_hasDeferred;
}

const std::vector<std::string> &ReceiverRestartPolicy::activeHosts() const
{
  return m_activeHosts;
}

const std::vector<std::string> &ReceiverRestartPolicy::deferredHosts() const
{
  return m_deferredHosts;
}

void ReceiverRestartPolicy::reset()
{
  m_activeHosts.clear();
  m_deferredHosts.clear();
  m_hasActive = false;
  m_hasDeferred = false;
}

void ReceiverAuthCache::pruneExpired(uint64_t timestampMs, uint64_t ttlMs)
{
  std::erase_if(m_entries, [timestampMs, ttlMs](const Entry &entry) {
    return timestampMs >= entry.timestampMs && (timestampMs - entry.timestampMs > ttlMs);
  });
}

void ReceiverAuthCache::record(const std::string &command, uint64_t timestampMs, uint64_t ttlMs)
{
  pruneExpired(timestampMs, ttlMs);
  if (command.rfind("GRANT ", 0) == 0) {
    std::erase_if(m_entries, [&command](const Entry &entry) { return entry.command == command; });
    while (m_entries.size() >= kMaxEntries) {
      m_entries.pop_front();
    }
    m_entries.push_back(Entry{command, timestampMs});
  } else if (command.rfind("REVOKE ", 0) == 0) {
    const std::string grantCmd = "GRANT " + command.substr(7);
    std::erase_if(m_entries, [&grantCmd](const Entry &entry) { return entry.command == grantCmd; });
  }
}

std::vector<std::string> ReceiverAuthCache::unexpiredGrants(uint64_t timestampMs, uint64_t ttlMs)
{
  pruneExpired(timestampMs, ttlMs);
  std::vector<std::string> out;
  out.reserve(m_entries.size());
  for (const auto &entry : m_entries) {
    out.push_back(entry.command);
  }
  return out;
}

void ReceiverAuthCache::clear()
{
  m_entries.clear();
}

// --- InProcessDropExecutor::SentinelBridge ---

struct InProcessDropExecutor::SentinelBridge
{
#if defined(_WIN32)
  static constexpr int kThickness = 2;
  static constexpr int kActivationZone = 16;

  class DropTarget final : public IDropTarget
  {
  public:
    SentinelBridge *bridge = nullptr;
    int sideIndex = -1;

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
      FORMATETC hdropFmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
      FORMATETC idListFmt = {
          static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_SHELLIDLIST)), nullptr, DVASPECT_CONTENT, -1,
          TYMED_HGLOBAL
      };
      const bool hasDropFormat =
          obj != nullptr && (obj->QueryGetData(&hdropFmt) == S_OK || obj->QueryGetData(&idListFmt) == S_OK);
      auto files = hasDropFormat ? extractFilesFromDataObject(obj) : std::vector<std::string>{};
      if (bridge && bridge->owner && hasDropFormat && !files.empty() && !bridge->cancellingNativeDrag.load()) {
        LOG_INFO("FileDropBridge: sentinel DragEnter at (%ld,%ld) captured %zu file(s)", pt.x, pt.y, files.size());
        bridge->owner->onSentinelDragEnter(sideIndex, std::move(files), pt.x, pt.y);
      }
      // 本地目标永远不接收原生放下，避免非文件拖放阻断及源文件误移动。
      *eff = DROPEFFECT_NONE;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *eff) override
    {
      *eff = DROPEFFECT_NONE;
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override
    {
      if (bridge && bridge->owner && !bridge->cancellingNativeDrag.load() &&
          !bridge->nativeDragCancellationComplete.load()) {
        bridge->owner->onSentinelDragLeave();
      }
      return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject *, DWORD, POINTL, DWORD *eff) override
    {
      *eff = DROPEFFECT_NONE;
      return S_OK;
    }
  };

  explicit SentinelBridge(InProcessDropExecutor *ownerPtr) : owner(ownerPtr) {}

  ~SentinelBridge()
  {
    shutdown();
  }

  static void CALLBACK onCancelTimer(HWND hwnd, UINT, UINT_PTR timerId, DWORD)
  {
    KillTimer(hwnd, timerId);
    auto *self = reinterpret_cast<SentinelBridge *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!self) {
      return;
    }
    self->cancelTimerWindow.store(nullptr);
    if (!self->cancellingNativeDrag.exchange(false)) {
      return;
    }
    sendEscapeKeyUp(kSyntheticMouseExtraInfo);
    self->nativeDragCancellationComplete.store(true);
    const bool stillPressed = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (self->owner) {
      self->owner->onSentinelCancellationCompleted(stillPressed);
    }
  }

  void ensureInitialized(int32_t x, int32_t y, int32_t w, int32_t h)
  {
    bool needUpdateGeometry = false;
    if (!initialized) {
      if (FAILED(OleInitialize(nullptr))) {
        return;
      }
      WNDCLASSW wc = {};
      wc.lpfnWndProc = DefWindowProcW;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"DeskflowFileDropSentinel";
      RegisterClassW(&wc);

      for (size_t i = 0; i < 4; ++i) {
        dropTargets[i].bridge = this;
        dropTargets[i].sideIndex = static_cast<int>(i);
        HWND hwnd = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, wc.lpszClassName, L"DeskflowSentinel",
            WS_POPUP, 0, 0, kThickness, kThickness, nullptr, nullptr, wc.hInstance, nullptr
        );
        if (hwnd && FAILED(RegisterDragDrop(hwnd, &dropTargets[i]))) {
          DestroyWindow(hwnd);
          continue;
        }
        if (hwnd) {
          SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
          SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
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
    lastEdgeRects[0] = {x, y, kThickness, h};
    lastEdgeRects[1] = {x + w - kThickness, y, kThickness, h};
    lastEdgeRects[2] = {x, y, w, kThickness};
    lastEdgeRects[3] = {x, y + h - kThickness, w, kThickness};
    for (size_t i = 0; i < 4; ++i) {
      if (hwnds[i]) {
        const auto &r = lastEdgeRects[i];
        SetWindowPos(
            hwnds[i], HWND_TOPMOST, r[0], r[1], r[2], r[3], SWP_NOACTIVATE | (visible[i] ? SWP_SHOWWINDOW : SWP_HIDEWINDOW)
        );
      }
    }
  }

  void setEdgeGeometryAndVisible(size_t idx, int32_t rx, int32_t ry, int32_t rw, int32_t rh, bool show)
  {
    if (!initialized || idx >= 4 || !hwnds[idx]) {
      return;
    }
    const std::array<int32_t, 4> targetRect = {rx, ry, std::max<int32_t>(1, rw), std::max<int32_t>(1, rh)};
    if (show) {
      if (lastEdgeRects[idx] != targetRect || !visible[idx]) {
        lastEdgeRects[idx] = targetRect;
        visible[idx] = true;
        SetWindowPos(
            hwnds[idx], HWND_TOPMOST, targetRect[0], targetRect[1], targetRect[2], targetRect[3],
            SWP_NOACTIVATE | SWP_SHOWWINDOW
        );
      }
    } else if (visible[idx]) {
      visible[idx] = false;
      ShowWindow(hwnds[idx], SW_HIDE);
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

  void clearCancellationFlags()
  {
    if (cancellingNativeDrag.exchange(false)) {
      if (HWND timerHwnd = cancelTimerWindow.exchange(nullptr)) {
        KillTimer(timerHwnd, 1);
      }
    }
    nativeDragCancellationComplete.store(false);
  }

  void cancelBeforeSwitch(int sideIndex)
  {
    if (sideIndex < 0 || sideIndex >= 4 || !initialized || !hwnds[static_cast<size_t>(sideIndex)]) {
      if (owner) {
        owner->onSentinelCancellationCompleted(false);
      }
      return;
    }
    const HWND hwnd = hwnds[static_cast<size_t>(sideIndex)];
    cancellingNativeDrag.store(true);
    cancellingSide.store(sideIndex);
    cancelTimerWindow.store(hwnd);
    if (!SetTimer(hwnd, 1, 80, onCancelTimer)) {
      cancellingNativeDrag.store(false);
      cancelTimerWindow.store(nullptr);
      LOG_WARN("FileDropBridge: could not schedule native drag cancellation");
      if (owner) {
        owner->onSentinelCancellationCompleted(false);
      }
      return;
    }
    INPUT escape = {};
    escape.type = INPUT_KEYBOARD;
    escape.ki.wVk = VK_ESCAPE;
    escape.ki.dwExtraInfo = kSyntheticMouseExtraInfo;
    if (SendInput(1, &escape, sizeof(escape)) != 1) {
      KillTimer(hwnd, 1);
      cancellingNativeDrag.store(false);
      cancelTimerWindow.store(nullptr);
      LOG_WARN("FileDropBridge: could not cancel source native drag");
      if (owner) {
        owner->onSentinelCancellationCompleted(false);
      }
    }
  }

  void shutdown()
  {
    if (cancellingNativeDrag.exchange(false)) {
      if (HWND timerHwnd = cancelTimerWindow.exchange(nullptr)) {
        KillTimer(timerHwnd, 1);
      }
      sendEscapeKeyUp(kSyntheticMouseExtraInfo);
    }
    nativeDragCancellationComplete.store(false);
    if (!initialized) {
      return;
    }
    for (size_t i = 0; i < 4; ++i) {
      if (hwnds[i]) {
        SetWindowLongPtrW(hwnds[i], GWLP_USERDATA, 0);
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

  InProcessDropExecutor *owner = nullptr;
  bool initialized = false;
  int32_t lastX = 0;
  int32_t lastY = 0;
  int32_t lastW = 0;
  int32_t lastH = 0;
  std::array<std::array<int32_t, 4>, 4> lastEdgeRects{};
  std::array<HWND, 4> hwnds = {nullptr, nullptr, nullptr, nullptr};
  std::array<bool, 4> visible = {false, false, false, false};
  std::array<DropTarget, 4> dropTargets;
  std::atomic<bool> cancellingNativeDrag{false};
  std::atomic<bool> nativeDragCancellationComplete{false};
  std::atomic<int> cancellingSide{0};
  std::atomic<HWND> cancelTimerWindow{nullptr};
#else
  static constexpr int kThickness = 2;
  static constexpr int kActivationZone = 16;

  explicit SentinelBridge(InProcessDropExecutor *owner)
  {
    host.setCallbacks({
        [owner, alive = owner->m_alive](int sideIndex, std::vector<std::string> files, int32_t x, int32_t y) {
          std::scoped_lock lock(alive->mutex);
          if (alive->load()) {
            owner->onSentinelDragEnter(sideIndex, std::move(files), x, y);
          }
        },
        [owner, alive = owner->m_alive]() {
          std::scoped_lock lock(alive->mutex);
          if (alive->load()) {
            owner->onSentinelDragLeave();
          }
        },
        [this, owner, alive = owner->m_alive](bool stillPressed) {
          std::scoped_lock lock(alive->mutex);
          if (alive->load()) {
            cancellingNativeDrag.store(false);
            nativeDragCancellationComplete.store(true);
            owner->onSentinelCancellationCompleted(stillPressed);
          }
        },
    });
  }

  ~SentinelBridge()
  {
    shutdown();
  }

  void ensureInitialized(int32_t x, int32_t y, int32_t w, int32_t h)
  {
    host.ensureInitialized(x, y, w, h);
  }

  void setEdgeGeometryAndVisible(size_t idx, int32_t x, int32_t y, int32_t w, int32_t h, bool show)
  {
    host.setEdgeGeometryAndVisible(idx, x, y, w, h, show);
  }

  void setEdgeVisible(size_t idx, bool show)
  {
    host.setEdgeVisible(idx, show);
  }

  void hideAll()
  {
    host.hideAll();
  }

  void clearCancellationFlags()
  {
    cancellingNativeDrag.store(false);
    nativeDragCancellationComplete.store(false);
    host.clearCancellationFlags();
  }

  void cancelBeforeSwitch(int sideIndex)
  {
    cancellingNativeDrag.store(true);
    host.cancelBeforeSwitch(sideIndex);
  }

  void shutdown()
  {
    host.shutdown();
  }

  std::atomic<bool> cancellingNativeDrag{false};
  std::atomic<bool> nativeDragCancellationComplete{false};
  platform_drop::QtEdgeSentinelHost host;
#endif
};

// --- InProcessDropExecutor ---

InProcessDropExecutor::InProcessDropExecutor(bool enableSentinels)
    : m_enableSentinels(enableSentinels),
      m_sentinels(std::make_unique<SentinelBridge>(this))
{
}

namespace {
void disposeProcess(QProcess *process)
{
  process->disconnect();
  if (process->state() != QProcess::NotRunning) {
    process->kill();
    process->waitForFinished(1000);
  }
  delete process;
}
} // namespace

InProcessDropExecutor::~InProcessDropExecutor()
{
  stopProcessesForShutdown();
  handleShutdownSentinels();
}

void InProcessDropExecutor::bindProcessToApplication(QProcess *process)
{
  auto *app = QCoreApplication::instance();
  if (!app) {
    return;
  }
  const auto cleanup = [this, alive = m_alive, process] {
    {
      std::scoped_lock lock(alive->mutex);
      if (alive->load()) {
        std::scoped_lock stateLock(m_mutex);
        if (m_receiverProcess == process) {
          m_receiverProcess = nullptr;
        }
        if (m_activeSendProcess == process) {
          m_activeSendProcess = nullptr;
        }
      }
    }
    disposeProcess(process);
  };
  QObject::connect(app, &QCoreApplication::aboutToQuit, process, cleanup);
  QObject::connect(app, &QObject::destroyed, process, cleanup);
}

void InProcessDropExecutor::stopProcessesForShutdown()
{
  const auto alive = m_alive;
  std::unique_lock lock(alive->mutex);
  alive->store(false);
  QProcess *receiver;
  QProcess *sender;
  {
    std::scoped_lock stateLock(m_mutex);
    receiver = std::exchange(m_receiverProcess, nullptr);
    sender = std::exchange(m_activeSendProcess, nullptr);
  }
  std::array<QProcess *, 2> localProcesses{};
  std::size_t localCount = 0;
  for (auto *process : {receiver, sender}) {
    if (!process) {
      continue;
    }
    if (QThread::currentThread() == process->thread()) {
      localProcesses[localCount++] = process;
    } else {
      QMetaObject::invokeMethod(
          process,
          [alive, process] {
            {
              std::scoped_lock lock(alive->mutex);
            }
            disposeProcess(process);
          },
          Qt::QueuedConnection
      );
    }
  }
  lock.unlock();
  for (std::size_t i = 0; i < localCount; ++i) {
    disposeProcess(localProcesses[i]);
  }
}

void InProcessDropExecutor::setHost(IDropExecutorHost *host)
{
  std::scoped_lock lock(m_alive->mutex);
  m_host.store(host);
}

void InProcessDropExecutor::execute(const DropAgentCommand &cmd)
{
  if (cmd.type == DropAgentCommandType::Shutdown) {
    stopProcessesForShutdown();
    handleShutdownSentinels();
    return;
  }
  const auto alive = m_alive;
  std::unique_lock<std::recursive_mutex> lifetimeLock(alive->mutex, std::defer_lock);
#if defined(_WIN32)
  if (!m_enableSentinels) {
    lifetimeLock.lock();
  }
#else
  lifetimeLock.lock();
#endif
  if (!alive->load()) {
    return;
  }
  switch (cmd.type) {
  case DropAgentCommandType::UpdatePrimaryShape:
    updatePrimaryShape(cmd.aw > 0 ? cmd.ax : cmd.x, cmd.aw > 0 ? cmd.ay : cmd.y, cmd.aw, cmd.ah);
    break;
  case DropAgentCommandType::PrimaryMouseMove:
    handlePrimaryMouseMove(cmd.x, cmd.y, cmd.ax, cmd.ay, cmd.aw, cmd.ah, cmd.activeSides, cmd.edgeSegments);
    break;
  case DropAgentCommandType::PrimaryLeftMouseDown:
    handlePrimaryLeftMouseDown();
    break;
  case DropAgentCommandType::LeftMouseUp:
    handleLeftMouseUp(cmd.flag);
    break;
  case DropAgentCommandType::HideSentinels:
    handleHideSentinels();
    break;
  case DropAgentCommandType::ShutdownSentinels:
    handleShutdownSentinels();
    break;
  case DropAgentCommandType::NudgeAcrossEdge:
    handleNudgeAcrossEdge(cmd.sideIndex);
    break;
  case DropAgentCommandType::CancelNativeDrag:
    handleCancelNativeDrag(cmd.sideIndex);
    break;
  case DropAgentCommandType::ShowHover:
    m_clientHoverIndicator.showHover(cmd.fileCount, cmd.x, cmd.y);
    break;
  case DropAgentCommandType::UpdateHoverCursor:
    m_clientHoverIndicator.updateCursor(cmd.x, cmd.y);
    break;
  case DropAgentCommandType::HideHover:
    m_clientHoverIndicator.hideHover();
    break;
  case DropAgentCommandType::BeginPreparing:
    m_receiverOverlay.beginPreparing(cmd.fileCount);
    break;
  case DropAgentCommandType::CancelPreparing:
    m_receiverOverlay.cancelPreparing();
    break;
  case DropAgentCommandType::TransferBegin:
    overlayFor(cmd.flag).begin(QString::fromStdString(cmd.text));
    break;
  case DropAgentCommandType::TransferProgress:
    overlayFor(cmd.flag).progress(cmd.done, cmd.total);
    break;
  case DropAgentCommandType::TransferFinish:
    {
      bool shouldCheckRestart = false;
      {
        std::scoped_lock lock(m_mutex);
        if (m_dragActive != cmd.dragActive) {
          m_dragActive = cmd.dragActive;
          shouldCheckRestart = !m_dragActive;
        }
      }
      overlayFor(cmd.flag).finish(cmd.success, QString::fromStdString(cmd.text));
      if (shouldCheckRestart) {
        maybeFlushDeferredRestart();
      }
    }
    break;
  case DropAgentCommandType::StartReceiver:
    startReceiverHosts(cmd.items);
    break;
  case DropAgentCommandType::StopReceiver:
    stopReceiver();
    break;
  case DropAgentCommandType::ReceiverAuthCommand:
    queueReceiverAuthCommand(cmd.text);
    break;
  case DropAgentCommandType::DropTarget:
    handleDropTarget(cmd.fileCount, cmd.x, cmd.y, cmd.text);
    break;
  case DropAgentCommandType::CancelDropToken:
    handleCancelDropToken(cmd.text, cmd.authToken);
    break;
  case DropAgentCommandType::SendFiles:
    sendFilesViaLocalSend(cmd.text, cmd.items, cmd.authToken);
    break;
  case DropAgentCommandType::CopyFilesToClipboard:
#if defined(_WIN32)
    copyFilesToWindowsClipboard(cmd.items);
#else
    platform_drop::copyFilesToPlatformClipboard(cmd.items);
#endif
    break;
  case DropAgentCommandType::SetDragActive:
    {
      bool flushNow = false;
      {
        std::scoped_lock lock(m_mutex);
        m_dragActive = cmd.dragActive;
        flushNow = !m_dragActive;
      }
      if (flushNow) {
        maybeFlushDeferredRestart();
      }
    }
    break;
  case DropAgentCommandType::Shutdown:
    break;
  }
}

bool InProcessDropExecutor::isHoverVisible() const
{
  return m_clientHoverIndicator.isVisible();
}

uint32_t InProcessDropExecutor::hoverFileCount() const
{
  return m_clientHoverIndicator.fileCount();
}

QPoint InProcessDropExecutor::hoverCursorPos() const
{
  return m_clientHoverIndicator.cursorPos();
}

TransferStatusOverlay::Phase InProcessDropExecutor::receiverOverlayPhase() const
{
  return m_receiverOverlay.phase();
}

uint32_t InProcessDropExecutor::receiverPreparingFileCount() const
{
  return m_receiverOverlay.preparingFileCount();
}

TransferStatusOverlay &InProcessDropExecutor::overlayFor(bool receiving)
{
  return receiving ? m_receiverOverlay : m_senderOverlay;
}

bool InProcessDropExecutor::hasActiveTransfers() const
{
  std::scoped_lock lock(m_mutex);
  return m_sendingInProgress || !m_sendQueue.empty() || m_activeReceiveSession;
}

void InProcessDropExecutor::updatePrimaryShape(int32_t x, int32_t y, int32_t w, int32_t h)
{
  if (m_enableSentinels && w > 0 && h > 0) {
    m_sentinels->ensureInitialized(x, y, w, h);
  }
}

void InProcessDropExecutor::handlePrimaryMouseMove(
    int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t aw, int32_t ah, uint32_t activeSides,
    const std::vector<DragEdgeSegment> &edgeSegments
)
{
#if !defined(_WIN32)
  if (!platform_drop::isNativeEdgeSentinelSupported()) {
    return;
  }
#endif
  if (!m_enableSentinels || aw <= 0 || ah <= 0) {
    return;
  }
  m_sentinels->ensureInitialized(ax, ay, aw, ah);
#if defined(_WIN32)
  const bool leftButtonDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#else
  const bool leftButtonDown = platform_drop::isNativeLeftMouseButtonDown(true);
#endif
  if (!leftButtonDown) {
    m_sentinels->hideAll();
    return;
  }

  const int32_t zone = SentinelBridge::kActivationZone;
  const bool nearSide[4] = {
      x < ax + zone,
      x >= ax + aw - zone,
      y < ay + zone,
      y >= ay + ah - zone,
  };

  if (!edgeSegments.empty()) {
    for (int side = 0; side < 4; ++side) {
      const DragEdgeSegment *matched = nullptr;
      const int32_t parallelLocal = (side <= 1) ? (y - ay) : (x - ax);
      if (nearSide[side]) {
        for (const auto &seg : edgeSegments) {
          if (static_cast<int>(seg.side) == side && parallelLocal >= seg.start && parallelLocal < seg.end) {
            matched = &seg;
            break;
          }
        }
      }
      if (!matched || matched->end <= matched->start) {
        m_sentinels->setEdgeVisible(static_cast<size_t>(side), false);
        continue;
      }
      const int32_t span = matched->end - matched->start;
      switch (side) {
      case 0:
        m_sentinels->setEdgeGeometryAndVisible(0, ax, ay + matched->start, SentinelBridge::kThickness, span, true);
        break;
      case 1:
        m_sentinels->setEdgeGeometryAndVisible(
            1, ax + aw - SentinelBridge::kThickness, ay + matched->start, SentinelBridge::kThickness, span, true
        );
        break;
      case 2:
        m_sentinels->setEdgeGeometryAndVisible(2, ax + matched->start, ay, span, SentinelBridge::kThickness, true);
        break;
      case 3:
        m_sentinels->setEdgeGeometryAndVisible(
            3, ax + matched->start, ay + ah - SentinelBridge::kThickness, span, SentinelBridge::kThickness, true
        );
        break;
      }
    }
    return;
  }

  using enum DirectionMask;
  const uint32_t sideMasks[4] = {
      static_cast<uint32_t>(LeftMask),
      static_cast<uint32_t>(RightMask),
      static_cast<uint32_t>(TopMask),
      static_cast<uint32_t>(BottomMask),
  };
  for (size_t side = 0; side < 4; ++side) {
    m_sentinels->setEdgeVisible(side, ((activeSides & sideMasks[side]) != 0) && nearSide[side]);
  }
}

void InProcessDropExecutor::handlePrimaryLeftMouseDown()
{
  if (m_enableSentinels) {
    m_sentinels->clearCancellationFlags();
  }
}

void InProcessDropExecutor::handleLeftMouseUp(bool releaseSourceButton)
{
#if defined(_WIN32)
  if (m_enableSentinels) {
    m_sentinels->hideAll();
    m_sentinels->clearCancellationFlags();
  }
  if (releaseSourceButton) {
    // 物理松手在远端时另给源系统补一次松手，清除本地按下状态。
    sendLeftButtonUp(kSyntheticMouseExtraInfo);
  }
#else
  if (m_enableSentinels) {
    m_sentinels->hideAll();
    m_sentinels->clearCancellationFlags();
  }
  if (releaseSourceButton) {
    platform_drop::postNativeLeftMouseUp();
  }
#endif
}

void InProcessDropExecutor::handleHideSentinels()
{
  if (m_enableSentinels) {
    m_sentinels->hideAll();
  }
}

void InProcessDropExecutor::handleShutdownSentinels()
{
  if (m_enableSentinels) {
    m_sentinels->shutdown();
  }
}

void InProcessDropExecutor::handleNudgeAcrossEdge(int sideIndex)
{
#if defined(_WIN32)
  if (!m_enableSentinels) {
    return;
  }
  m_sentinels->cancellingSide.store(sideIndex);
  m_sentinels->nativeDragCancellationComplete.store(false);
  const int dx = sideIndex == 0 ? -8 : sideIndex == 1 ? 8 : 0;
  const int dy = sideIndex == 2 ? -8 : sideIndex == 3 ? 8 : 0;
  sendRelativeMouseMove(dx, dy);
#else
  if (m_enableSentinels && platform_drop::isNativeEdgeSentinelSupported()) {
    const int dx = sideIndex == 0 ? -8 : sideIndex == 1 ? 8 : 0;
    const int dy = sideIndex == 2 ? -8 : sideIndex == 3 ? 8 : 0;
    platform_drop::postNativeRelativeMouseMove(dx, dy);
  }
#endif
}

void InProcessDropExecutor::handleCancelNativeDrag(int sideIndex)
{
  if (!m_enableSentinels) {
    onSentinelCancellationCompleted(false);
    return;
  }
  m_sentinels->cancelBeforeSwitch(sideIndex);
}

void InProcessDropExecutor::onSentinelDragEnter(int sideIndex, std::vector<std::string> files, int32_t x, int32_t y)
{
  std::scoped_lock lifetimeLock(m_alive->mutex);
  if (!m_alive->load()) {
    return;
  }
  if (auto *host = m_host.load()) {
    host->onPendingDragCaptured(std::move(files), sideIndex, x, y);
  }
}

void InProcessDropExecutor::onSentinelDragLeave()
{
  std::scoped_lock lifetimeLock(m_alive->mutex);
  if (!m_alive->load()) {
    return;
  }
  if (auto *host = m_host.load()) {
    host->onPendingDragCleared();
  }
}

void InProcessDropExecutor::onSentinelCancellationCompleted(bool stillPressed)
{
  std::scoped_lock lifetimeLock(m_alive->mutex);
  if (!m_alive->load()) {
    return;
  }
  if (auto *host = m_host.load()) {
    host->onDragCancellationCompleted(stillPressed);
  }
}

std::string InProcessDropExecutor::findLocalSendCliPath()
{
  if (const char *envPath = std::getenv("DESKSEND_LOCALSEND_CLI"); envPath && envPath[0] != '\0') {
#if defined(_WIN32)
    if (QFileInfo::exists(QString::fromUtf8(envPath))) {
      return envPath;
#else
    const QFileInfo overridePath(QString::fromUtf8(envPath));
    if (overridePath.isFile() && overridePath.isExecutable()) {
      return overridePath.absoluteFilePath().toStdString();
#endif
    }
  }
  const QString appDir = QCoreApplication::applicationDirPath();
#if defined(_WIN32)
  const QString exeName = QStringLiteral("localsend-cli.exe");
#else
  const QString exeName = QStringLiteral("localsend-cli");
#endif
  const QString candidate = QDir(appDir).filePath(exeName);
#if defined(_WIN32)
  if (QFileInfo::exists(candidate)) {
    return candidate.toStdString();
  }
  return exeName.toStdString();
#else
  const QFileInfo bundled(candidate);
  if (bundled.isFile() && bundled.isExecutable()) {
    return candidate.toStdString();
  }
  const auto executable = QStandardPaths::findExecutable(exeName);
  return executable.isEmpty() ? exeName.toStdString() : executable.toStdString();
#endif
}

std::string InProcessDropExecutor::computeTokenTag(const std::string &token)
{
  if (token.empty()) {
    return {};
  }
  const QByteArray digest = QCryptographicHash::hash(
      QByteArray::fromRawData(token.data(), static_cast<qsizetype>(token.size())), QCryptographicHash::Sha256
  );
  return digest.toHex().left(16).toStdString();
}

std::string InProcessDropExecutor::uniqueDestinationPath(const std::string &dstDir, const std::string &relPath)
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
  while (QFileInfo::exists(dstPath) || QFileInfo(dstPath).isSymLink()) {
    if (suffix.isEmpty()) {
      dstPath = dstParentDir.filePath(QStringLiteral("%1 (%2)").arg(baseName).arg(counter));
    } else {
      dstPath = dstParentDir.filePath(QStringLiteral("%1 (%2).%3").arg(baseName).arg(counter).arg(suffix));
    }
    ++counter;
  }
  return dstPath.toStdString();
}

std::string InProcessDropExecutor::resolveDropTargetDirectory(
    int32_t screenX, int32_t screenY, const std::string &fallbackDir, bool *outIsDirectFolderTarget
)
{
  if (outIsDirectFolderTarget) {
    *outIsDirectFolderTarget = true;
  }
  if (const char *forcedDir = std::getenv("DESKSEND_DEFAULT_DROP_DIR"); forcedDir && forcedDir[0] != '\0') {
    return forcedDir;
  }

#if defined(_WIN32)
  struct ComScope
  {
    HRESULT hr;
    ComScope() : hr(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
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
  return fallbackDir;
#else
  return platform_drop::resolveNonWindowsDropTargetDirectory(screenX, screenY, fallbackDir, outIsDirectFolderTarget);
#endif
}

void InProcessDropExecutor::notifyActiveTransfersChangedLocked(bool &outShouldNotify, bool &outActive)
{
  const bool active = m_sendingInProgress || !m_sendQueue.empty() || m_activeReceiveSession;
  if (active == m_lastReportedActiveTransfers) {
    outShouldNotify = false;
    outActive = active;
    return;
  }
  m_lastReportedActiveTransfers = active;
  outShouldNotify = true;
  outActive = active;
}

void InProcessDropExecutor::emitActiveTransfersChanged(bool active)
{
  std::scoped_lock lifetimeLock(m_alive->mutex);
  if (!m_alive->load()) {
    return;
  }
  LOG_INFO("FileDropBridge: activeTransfers=%d", active ? 1 : 0);
  ipcSendToClient(QStringLiteral("activeTransfers"), active ? QStringLiteral("1") : QStringLiteral("0"));
  if (auto *host = m_host.load()) {
    host->onActiveTransfersChanged(active);
  }
  if (!active) {
    maybeFlushDeferredRestart();
  }
}

void InProcessDropExecutor::maybeFlushDeferredRestart()
{
  std::optional<std::vector<std::string>> pendingHosts;
  {
    std::scoped_lock lock(m_mutex);
    const bool transferActive = m_sendingInProgress || !m_sendQueue.empty() || m_activeReceiveSession;
    pendingHosts = m_restartPolicy.onActivityChanged(m_dragActive, transferActive);
  }
  if (pendingHosts.has_value()) {
    if (pendingHosts->empty()) {
      stopReceiver();
    } else {
      startReceiverInternal(*pendingHosts, false);
    }
  }
}

void InProcessDropExecutor::startReceiverHosts(const std::vector<std::string> &hosts)
{
  std::optional<std::vector<std::string>> decision;
  {
    std::scoped_lock lock(m_mutex);
    const bool transferActive = m_sendingInProgress || !m_sendQueue.empty() || m_activeReceiveSession;
    decision = m_restartPolicy.requestHosts(hosts, m_dragActive, transferActive);
  }
  if (!decision.has_value()) {
    return;
  }
  if (decision->empty()) {
    stopReceiver();
    return;
  }
  startReceiverInternal(*decision, false);
}

void InProcessDropExecutor::startReceiverInternal(const std::vector<std::string> &hosts, bool isRestart)
{
  std::scoped_lock lifetimeLock(m_alive->mutex);
  if (!m_alive->load()) {
    return;
  }
  if (auto *app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
    const auto alive = m_alive;
    QMetaObject::invokeMethod(
        app,
        [this, alive, hosts, isRestart]() {
          std::scoped_lock lifetimeLock(alive->mutex);
          if (!alive->load()) {
            return;
          }
          if (alive->load()) {
            startReceiverInternal(hosts, isRestart);
          }
        },
        Qt::QueuedConnection
    );
    return;
  }

  const std::vector<std::string> normalized = ReceiverRestartPolicy::normalizeHosts(hosts);
  const std::string hostsKey = joinHostsKey(normalized);
  if (hostsKey.empty()) {
    return;
  }

  QProcess *oldProc = nullptr;
  QString staging;
  uint32_t generation = 0;
  bool shouldNotify = false;
  bool notifyActive = false;
  {
    std::scoped_lock lock(m_mutex);
    if (isRestart && (!m_receiverDesired || m_currentAllowedHostsKey != hostsKey)) {
      return;
    }
    if (!isRestart && m_receiverProcess && m_receiverProcess->state() != QProcess::NotRunning &&
        m_currentAllowedHostsKey == hostsKey && m_receiverDesired) {
      return;
    }
    oldProc = m_receiverProcess;
    m_receiverProcess = nullptr;
    ++m_receiverGeneration;
    generation = m_receiverGeneration;
    m_receiverDesired = true;
    m_currentAllowedHostsKey = hostsKey;
    if (!isRestart) {
      m_receiverRestartAttempt = 0;
      m_pendingReceiverAuthLines.clear();
      m_cachedReceiverGrants.clear();
      m_pendingDropTargets.clear();
      m_activeSessionTarget = DropTargetEntry{};
      m_activeReceiveSessionId.clear();
      m_activeReceiveSession = false;
      notifyActiveTransfersChangedLocked(shouldNotify, notifyActive);
    } else {
      const auto cachedGrants = m_cachedReceiverGrants.unexpiredGrants(nowMs());
      for (auto it = cachedGrants.rbegin(); it != cachedGrants.rend(); ++it) {
        if (std::find(m_pendingReceiverAuthLines.begin(), m_pendingReceiverAuthLines.end(), *it) ==
            m_pendingReceiverAuthLines.end()) {
          m_pendingReceiverAuthLines.push_front(*it);
        }
      }
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
  if (shouldNotify) {
    emitActiveTransfersChanged(notifyActive);
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
  const QString allowSpec = buildAllowSpecForHosts(normalized);

  QStringList args;
  args << QStringLiteral("--port") << port << QStringLiteral("--destination") << staging << QStringLiteral("receive")
       << QStringLiteral("--auto-accept-from") << allowSpec << QStringLiteral("--auth-stdin") << QStringLiteral("--received-paths-hex");

  auto *proc = new QProcess();
  bindProcessToApplication(proc);
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

  QObject::connect(proc, &QProcess::started, proc, [this, alive = m_alive, proc, generation]() {
    std::scoped_lock lifetimeLock(alive->mutex);
    if (!alive->load()) {
      return;
    }
    std::deque<std::string> pending;
    {
      std::scoped_lock lock(m_mutex);
      if (generation != m_receiverGeneration || m_receiverProcess != proc) {
        return;
      }
      pending.swap(m_pendingReceiverAuthLines);
    }
#if defined(_WIN32)
    configureLocalSendChildProcess(proc->processId());
#endif
    for (const auto &cmd : pending) {
      const QByteArray line = QByteArray::fromStdString(cmd + "\n");
      proc->write(line);
    }
  });

  QObject::connect(proc, &QProcess::errorOccurred, proc, [this, alive = m_alive, proc, generation](QProcess::ProcessError error) {
    std::scoped_lock lifetimeLock(alive->mutex);
    if (!alive->load()) {
      return;
    }
    if (error == QProcess::FailedToStart) {
      bool shouldWarn = false;
      {
        std::scoped_lock lock(m_mutex);
        if (m_receiverProcess == proc) {
          m_receiverProcess = nullptr;
        }
        shouldWarn = (generation == m_receiverGeneration);
        if (shouldWarn) {
          m_receiverDesired = false;
        }
      }
      proc->deleteLater();
      if (shouldWarn) {
        m_receiverOverlay.finish(
#if defined(_WIN32)
            false, QCoreApplication::translate("FileDropBridge", "Receiver failed to start: unable to launch localsend-cli.exe")
#else
            false, QCoreApplication::translate("FileDropBridge", "Receiver failed to start: unable to launch localsend-cli")
#endif
        );
      }
    }
  });

  QObject::connect(
      proc, &QProcess::finished, proc, [this, alive = m_alive, proc, generation, normalized, hostsKey](int exitCode, QProcess::ExitStatus exitStatus) {
        std::scoped_lock lifetimeLock(alive->mutex);
        if (!alive->load()) {
          return;
        }
        proc->deleteLater();
        int delayMs = 0;
        int attempt = 0;
        bool shouldRestart = false;
        bool cappedRestart = false;
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
            if (m_receiverDesired && m_currentAllowedHostsKey == hostsKey) {
              if (m_receiverRestartAttempt < kMaxConsecutiveReceiverRestarts) {
                shouldRestart = true;
                delayMs = receiverRestartDelayMs(m_receiverRestartAttempt);
                ++m_receiverRestartAttempt;
                attempt = m_receiverRestartAttempt;
              } else {
                m_receiverDesired = false;
                cappedRestart = true;
              }
            }
          }
        }
        if (!currentGen) {
          return;
        }
        if (hadActiveSession) {
          handleReceiveSessionFinished(true);
        }
        if (cappedRestart) {
          LOG_WARN(
              "FileDropBridge: receiver exited unexpectedly %d consecutive times (last code=%d, status=%d); stopping automatic recovery",
              kMaxConsecutiveReceiverRestarts, exitCode, static_cast<int>(exitStatus)
          );
        }
        m_receiverOverlay.finish(
            false,
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
            const auto alive = m_alive;
            QTimer::singleShot(delayMs, app, [this, alive, generation, normalized, hostsKey]() {
              std::scoped_lock lifetimeLock(alive->mutex);
              if (!alive->load()) {
                return;
              }
              {
                std::scoped_lock lock(m_mutex);
                if (!m_receiverDesired || generation != m_receiverGeneration || m_currentAllowedHostsKey != hostsKey) {
                  return;
                }
              }
              startReceiverInternal(normalized, true);
            });
          }
        }
      }
  );

  QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, [this, alive = m_alive, proc, generation]() {
    std::scoped_lock lifetimeLock(alive->mutex);
    if (!alive->load()) {
      return;
    }
    while (proc->canReadLine()) {
      auto rawLine = proc->readLine();
      if (rawLine.endsWith('\n')) {
        rawLine.chop(1);
      }
#if defined(_WIN32)
      if (rawLine.endsWith('\r')) {
        rawLine.chop(1);
      }
#endif
      const QString line = QString::fromUtf8(rawLine);
      if (!line.startsWith(QStringLiteral("RECEIVE_PROGRESS:"))) {
        LOG_INFO("FileDropBridge [rx]: %s", line.toStdString().c_str());
      }
      {
        std::scoped_lock lock(m_mutex);
        if (generation != m_receiverGeneration) {
          return;
        }
        if (line.startsWith(QStringLiteral("LOCALSEND_RECEIVER_READY port="))) {
          m_receiverRestartAttempt = 0;
        }
      }
      const QString filePrefix = QStringLiteral("RECEIVED_FILE_HEX:");
      const QString startPrefix = QStringLiteral("RECEIVE_SESSION_STARTED:");
      if (line.startsWith(QStringLiteral("RECEIVE_PROGRESS:"))) {
        const auto fields = line.split(':');
        if (fields.size() == 4) {
          const std::string sessionId = fields[1].toStdString();
          bool current;
          {
            std::scoped_lock lock(m_mutex);
            current = m_activeReceiveSessionId == sessionId;
          }
          if (current) {
            m_receiverOverlay.progress(fields[2].toULongLong(), fields[3].toULongLong());
          }
        }
      } else if (line.startsWith(filePrefix)) {
        const auto encoded = line.mid(filePrefix.size()).toLatin1();
        const auto path = QByteArray::fromHex(encoded);
        if (path.isEmpty() || path.contains('\0') || path.toHex() != encoded ||
            QString::fromUtf8(path).toUtf8() != path) {
          handleReceiveFileFailedLine();
        } else {
          handleReceivedFileLine(path.toStdString());
        }
      } else if (line.startsWith(QStringLiteral("RECEIVE_FILE_FAILED:"))) {
        handleReceiveFileFailedLine();
      } else if (line.startsWith(startPrefix)) {
        QString header = line;
        std::string tokenTag;
        const qsizetype tokSep = header.lastIndexOf(QStringLiteral(":tok="));
        if (tokSep > 0) {
          tokenTag = header.mid(tokSep + 5).toStdString();
          header = header.left(tokSep);
        }
        size_t expectedCount = 0;
        const qsizetype lastColon = header.lastIndexOf(':');
        if (lastColon > 0) {
          expectedCount = static_cast<size_t>(header.mid(lastColon + 1).toUInt());
        }
        handleReceiveSessionStarted(expectedCount, tokenTag, header.section(':', 1, 1).toStdString());
      } else if (line.startsWith(QStringLiteral("RECEIVE_SESSION_FINISHED:"))) {
        handleReceiveSessionFinished(false, line.section(':', 1, 1).toStdString());
      } else if (line.startsWith(QStringLiteral("RECEIVE_SESSION_CANCELLED:"))) {
        handleReceiveSessionFinished(true, line.section(':', 1, 1).toStdString());
      }
    }
  });

  LOG_INFO(
      "FileDropBridge: starting headless receiver: %s %s", cliPath.toStdString().c_str(),
      args.join(' ').toStdString().c_str()
  );
  proc->start(cliPath, args);
}

void InProcessDropExecutor::stopReceiver()
{
  std::scoped_lock lifetimeLock(m_alive->mutex);
  if (!m_alive->load()) {
    return;
  }
  if (auto *app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
    const auto alive = m_alive;
    QMetaObject::invokeMethod(
        app,
        [this, alive]() {
          std::scoped_lock lifetimeLock(alive->mutex);
          if (!alive->load()) {
            return;
          }
          if (alive->load()) {
            stopReceiver();
          }
        },
        Qt::QueuedConnection
    );
    return;
  }

  QProcess *oldProc = nullptr;
  bool shouldNotify = false;
  bool notifyActive = false;
  {
    std::scoped_lock lock(m_mutex);
    m_restartPolicy.reset();
    m_receiverDesired = false;
    ++m_receiverGeneration;
    m_receiverRestartAttempt = 0;
    m_currentAllowedHostsKey.clear();
    m_pendingReceiverAuthLines.clear();
    m_cachedReceiverGrants.clear();
    m_pendingDropTargets.clear();
    m_activeSessionTarget = DropTargetEntry{};
    m_activeReceiveSessionId.clear();
    m_activeSessionPlacedFiles.clear();
    m_activeSessionExpectedFiles = 0;
    m_activeSessionHandledFiles = 0;
    m_activeSessionEnded = false;
    m_activeSessionFailed = false;
    m_activeSessionCancelled = false;
    m_activeReceiveSession = false;
    notifyActiveTransfersChangedLocked(shouldNotify, notifyActive);
    oldProc = m_receiverProcess;
    m_receiverProcess = nullptr;
  }
  if (shouldNotify) {
    emitActiveTransfersChanged(notifyActive);
  }
  if (oldProc) {
    oldProc->disconnect();
    oldProc->kill();
    oldProc->waitForFinished(1000);
    delete oldProc;
  }
  if (m_receiverOverlay.phase() == TransferStatusOverlay::Phase::Preparing ||
      m_receiverOverlay.phase() == TransferStatusOverlay::Phase::Active) {
    m_receiverOverlay.resetToIdle();
  }
}

void InProcessDropExecutor::queueReceiverAuthCommand(const std::string &command)
{
  if (auto *app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
    const auto alive = m_alive;
    QMetaObject::invokeMethod(
        app,
        [this, alive, command]() {
          std::scoped_lock lifetimeLock(alive->mutex);
          if (!alive->load()) {
            return;
          }
          if (alive->load()) {
            queueReceiverAuthCommand(command);
          }
        },
        Qt::QueuedConnection
    );
    return;
  }

  std::scoped_lock lock(m_mutex);
  if (!m_receiverDesired) {
    return;
  }
  m_cachedReceiverGrants.record(command, nowMs());
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

void InProcessDropExecutor::handleDropTarget(uint32_t fileCount, int32_t x, int32_t y, const std::string &tokenTag)
{
  std::string fallback;
  {
    std::scoped_lock lock(m_mutex);
    if (m_defaultDropFallbackDir.empty()) {
      QString defaultDrop = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
      if (defaultDrop.isEmpty()) {
        defaultDrop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
      }
      if (const char *envDrop = std::getenv("DESKSEND_DEFAULT_DROP_DIR"); envDrop && envDrop[0] != '\0') {
        defaultDrop = QString::fromUtf8(envDrop);
      }
      m_defaultDropFallbackDir = defaultDrop.toStdString();
    }
    fallback = m_defaultDropFallbackDir;
  }

  bool isDirectFolder = false;
  const bool isDirectSend = x == kDirectSendDropCoord && y == kDirectSendDropCoord;
  std::string resolved = isDirectSend ? fallback : resolveDropTargetDirectory(x, y, fallback, &isDirectFolder);
  {
    std::scoped_lock lock(m_mutex);
    const uint64_t now = nowMs();
    while (!m_pendingDropTargets.empty() && now >= m_pendingDropTargets.front().timestampMs &&
           (now - m_pendingDropTargets.front().timestampMs > 300000)) {
      m_pendingDropTargets.pop_front();
    }
    m_pendingDropTargets.push_back(DropTargetEntry{resolved, !isDirectFolder, now, tokenTag});
  }
  LOG_INFO(
      "FileDropBridge: remote drop at (%d,%d) for %u file(s) -> target folder \"%s\" (directFolder=%d)", x, y,
      fileCount, resolved.c_str(), isDirectFolder ? 1 : 0
  );
}

void InProcessDropExecutor::handleCancelDropToken(const std::string &tokenTag, const std::string &authToken)
{
  if (!authToken.empty() && authToken.find_first_of(" \t\r\n") == std::string::npos) {
    queueReceiverAuthCommand("REVOKE " + authToken);
  }
  if (!tokenTag.empty()) {
    std::scoped_lock lock(m_mutex);
    std::erase_if(m_pendingDropTargets, [&tokenTag](const DropTargetEntry &entry) {
      return entry.tokenTag == tokenTag;
    });
  }
}

void InProcessDropExecutor::sendFilesViaLocalSend(
    const std::string &targetIp, const std::vector<std::string> &files, const std::string &authToken
)
{
  std::scoped_lock lifetimeLock(m_alive->mutex);
  if (!m_alive->load()) {
    return;
  }
  if (auto *app = QCoreApplication::instance(); app && QThread::currentThread() != app->thread()) {
    const auto alive = m_alive;
    QMetaObject::invokeMethod(
        app,
        [this, alive, targetIp, files, authToken]() {
          std::scoped_lock lifetimeLock(alive->mutex);
          if (!alive->load()) {
            return;
          }
          if (alive->load()) {
            sendFilesViaLocalSend(targetIp, files, authToken);
          }
        },
        Qt::QueuedConnection
    );
    return;
  }

  bool shouldNotify = false;
  bool notifyActive = false;
  bool shouldStart = false;
  {
    std::scoped_lock lock(m_mutex);
    m_sendQueue.push_back(SendJob{targetIp, files, authToken, std::chrono::steady_clock::now()});
    if (!m_sendingInProgress) {
      m_sendingInProgress = true;
      shouldStart = true;
      notifyActiveTransfersChangedLocked(shouldNotify, notifyActive);
    } else {
      notifyActiveTransfersChangedLocked(shouldNotify, notifyActive);
      LOG_INFO(
          "FileDropBridge: queued send job to %s (%zu file(s)); %zu job(s) waiting", targetIp.c_str(), files.size(),
          m_sendQueue.size()
      );
    }
  }
  if (shouldNotify) {
    emitActiveTransfersChanged(notifyActive);
  }
  if (shouldStart) {
    processNextSendJob();
  }
}

void InProcessDropExecutor::processNextSendJob()
{
  std::scoped_lock lifetimeLock(m_alive->mutex);
  if (!m_alive->load()) {
    return;
  }
  SendJob job;
  bool shouldNotify = false;
  bool notifyActive = false;
  {
    std::scoped_lock lock(m_mutex);
    if (m_sendQueue.empty()) {
      m_sendingInProgress = false;
      notifyActiveTransfersChangedLocked(shouldNotify, notifyActive);
      if (!shouldNotify) {
        return;
      }
    } else {
      job = std::move(m_sendQueue.front());
      m_sendQueue.pop_front();
      notifyActiveTransfersChangedLocked(shouldNotify, notifyActive);
    }
  }
  if (shouldNotify) {
    emitActiveTransfersChanged(notifyActive);
  }
  if (job.targetIp.empty() && job.files.empty()) {
    return;
  }

  // 与 LocalSend 接收端一次性授权的有效期一致。
  constexpr auto authLifetime = std::chrono::minutes(5);
  const bool expired = std::chrono::steady_clock::now() - job.queuedAt >= authLifetime;
  if (job.authToken.empty() || expired) {
    const char *reason = expired ? "queued authorization expired" : "missing authorization token";
    LOG_WARN("FileDropBridge: %s for send job to %s; aborting", reason, job.targetIp.c_str());
    m_senderOverlay.finish(
        false, expired ? QCoreApplication::translate("FileDropBridge", "Transfer authorization expired while waiting. Send the files again.")
                       : QCoreApplication::translate("FileDropBridge", "Send failed: missing transfer authorization token")
    );
    if (auto *host = m_host.load()) {
      host->onSendCompleted(job.targetIp, false);
    }
    processNextSendJob();
    return;
  }

  const QString cliPath = QString::fromStdString(findLocalSendCliPath());
  QString targetPortStr = QStringLiteral("53318");
  if (const char *targetPort = std::getenv("DESKSEND_TARGET_PORT"); targetPort && targetPort[0] != '\0') {
    targetPortStr = QString::fromUtf8(targetPort);
  }
  const QString destination = formatHostPortTarget(job.targetIp, targetPortStr);

  QStringList args;
  if (const char *senderPort = std::getenv("DESKSEND_SENDER_PORT"); senderPort && senderPort[0] != '\0') {
    args << QStringLiteral("--port") << QString::fromUtf8(senderPort);
  } else {
    args << QStringLiteral("--port") << QStringLiteral("0");
  }
  args << QStringLiteral("send") << QStringLiteral("--to") << destination << QStringLiteral("--pin-stdin")
       << QStringLiteral("--paths-stdin") << QStringLiteral("--paths-stdin-hex") << QStringLiteral("--cancel-stdin");

  QByteArray stdinPayload = QByteArray::fromStdString(job.authToken + "\n");
  for (const auto &f : job.files) {
    const QByteArray line = QByteArray::fromStdString(f).toHex();
    if (!line.isEmpty()) {
      stdinPayload.append(line);
      stdinPayload.append('\n');
    }
  }
  stdinPayload.append('\n');

  auto *proc = new QProcess();
  bindProcessToApplication(proc);
  auto *inactivityTimer = new QTimer(proc);
  inactivityTimer->setSingleShot(true);
  const int inactivityMs = sendInactivityTimeoutMs();
  auto timedOut = std::make_shared<bool>(false);
  auto cancellationReported = std::make_shared<bool>(false);
  {
    std::scoped_lock lock(m_mutex);
    m_activeSendProcess = proc;
    m_sendCancelRequested = false;
  }
  proc->setProcessChannelMode(QProcess::MergedChannels);
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
  const QString senderCfgDir = resolveLocalSendConfigDir("DESKSEND_SENDER_CONFIG_HOME", QStringLiteral("sender"));
  QDir().mkpath(senderCfgDir);
  env.insert(QStringLiteral("XDG_CONFIG_HOME"), senderCfgDir);
  proc->setProcessEnvironment(env);
  const auto alive = m_alive;
  m_senderOverlay.begin(
      QCoreApplication::translate("FileDropBridge", "Sending to %1").arg(destination),
      [this, alive, process = QPointer<QProcess>(proc)] {
        std::scoped_lock lifetimeLock(alive->mutex);
        if (!alive->load()) {
          return;
        }
        if (alive->load() && process) {
          cancelActiveSend(process);
        }
      }
  );
  QObject::connect(inactivityTimer, &QTimer::timeout, proc, [alive, proc, timedOut, destination, inactivityMs]() {
    if (!alive->load()) {
      return;
    }
    *timedOut = true;
    LOG_WARN(
        "FileDropBridge: LocalSend transfer to %s stalled (no progress for %d ms); terminating",
        destination.toStdString().c_str(), inactivityMs
    );
    if (proc->state() != QProcess::NotRunning) {
      proc->kill();
    }
  });
  QObject::connect(proc, &QProcess::readyReadStandardOutput, proc, [this, alive, proc, inactivityTimer, inactivityMs, cancellationReported]() {
    std::scoped_lock lifetimeLock(alive->mutex);
    if (!alive->load()) {
      return;
    }
    inactivityTimer->start(inactivityMs);
    while (proc->canReadLine()) {
      const QString line = QString::fromUtf8(proc->readLine()).trimmed();
      if (line.startsWith(QStringLiteral("SEND_PROGRESS:"))) {
        const auto fields = line.split(':');
        if (fields.size() == 3) {
          m_senderOverlay.progress(fields[1].toULongLong(), fields[2].toULongLong());
        }
      } else if (line == QStringLiteral("SEND_CANCELLED")) {
        *cancellationReported = true;
      } else {
        LOG_INFO("FileDropBridge [tx]: %s", line.toStdString().c_str());
      }
    }
  });

  QObject::connect(proc, &QProcess::started, proc, [this, alive, proc, stdinPayload, inactivityTimer, inactivityMs]() {
    std::scoped_lock lifetimeLock(alive->mutex);
    if (!alive->load()) {
      return;
    }
#if defined(_WIN32)
    configureLocalSendChildProcess(proc->processId());
#endif
    inactivityTimer->start(inactivityMs);
    proc->write(stdinPayload);
    if (alive->load()) {
      std::scoped_lock lock(m_mutex);
      if (m_activeSendProcess == proc && m_sendCancelRequested) {
        proc->write("CANCEL\n");
      }
    }
  });

  const std::string jobTargetIp = job.targetIp;
  QObject::connect(
      proc, &QProcess::errorOccurred, proc,
      [this, alive, proc, inactivityTimer, destination, jobTargetIp](QProcess::ProcessError error) {
        std::scoped_lock lifetimeLock(alive->mutex);
        if (!alive->load()) {
          return;
        }
        if (!alive->load()) {
          inactivityTimer->stop();
          proc->deleteLater();
          return;
        }
        if (error == QProcess::FailedToStart) {
          inactivityTimer->stop();
          {
            std::scoped_lock lock(m_mutex);
            if (m_activeSendProcess == proc) {
              m_activeSendProcess = nullptr;
            }
            m_sendCancelRequested = false;
          }
          LOG_WARN("FileDropBridge: failed to start localsend-cli for %s", destination.toStdString().c_str());
          m_senderOverlay.finish(
#if defined(_WIN32)
              false, QCoreApplication::translate("FileDropBridge", "Send failed: unable to launch localsend-cli.exe")
#else
              false, QCoreApplication::translate("FileDropBridge", "Send failed: unable to launch localsend-cli")
#endif
          );
          if (auto *host = m_host.load()) {
            host->onSendCompleted(jobTargetIp, false);
          }
          proc->deleteLater();
          processNextSendJob();
        }
      }
  );

  QObject::connect(
      proc, &QProcess::finished, proc,
      [this, alive, proc, inactivityTimer, timedOut, cancellationReported, destination, jobTargetIp](
          int exitCode, QProcess::ExitStatus status
      ) {
        std::scoped_lock lifetimeLock(alive->mutex);
        if (!alive->load()) {
          return;
        }
        inactivityTimer->stop();
        proc->deleteLater();
        if (!alive->load()) {
          return;
        }
        bool cancelRequested;
        {
          std::scoped_lock lock(m_mutex);
          if (m_activeSendProcess == proc) {
            m_activeSendProcess = nullptr;
          }
          cancelRequested = m_sendCancelRequested;
          m_sendCancelRequested = false;
        }
        const QString out = QString::fromUtf8(proc->readAllStandardOutput()).trimmed();
        const bool success = !(*timedOut) && exitCode == 0 && status == QProcess::NormalExit;
        if (success) {
          m_senderOverlay.finish(true, QCoreApplication::translate("FileDropBridge", "File transfer completed"));
          LOG_INFO("FileDropBridge: LocalSend transfer to %s succeeded", destination.toStdString().c_str());
        } else if (cancelRequested || *cancellationReported || std::ranges::any_of(out.split('\n'), [](const QString &line) {
                     return line.trimmed() == QStringLiteral("SEND_CANCELLED");
                   })) {
          m_senderOverlay.finishCancelled();
          LOG_INFO("FileDropBridge: LocalSend transfer to %s cancelled", destination.toStdString().c_str());
        } else {
          m_senderOverlay.finish(false, QCoreApplication::translate("FileDropBridge", "File transfer failed"));
          LOG_WARN(
              "FileDropBridge: LocalSend transfer to %s failed (timedOut=%d exit=%d) out=\"%s\"",
              destination.toStdString().c_str(), *timedOut ? 1 : 0, exitCode, out.toStdString().c_str()
          );
        }
        if (auto *host = m_host.load()) {
          host->onSendCompleted(jobTargetIp, success);
        }
        processNextSendJob();
      }
  );

  LOG_INFO("FileDropBridge: spawning %s %s", cliPath.toStdString().c_str(), args.join(' ').toStdString().c_str());
  inactivityTimer->start(inactivityMs);
  proc->start(cliPath, args);
}

void InProcessDropExecutor::cancelActiveSend(QProcess *process)
{
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeSendProcess != process || m_sendCancelRequested || process->state() == QProcess::NotRunning) {
      return;
    }
    m_sendCancelRequested = true;
  }

  // 先让 CLI 通知接收端取消，仅在 CLI 无法及时退出时结束该发送进程。
  if (process->state() == QProcess::Running) {
    process->write("CANCEL\n");
  }
  QTimer::singleShot(8000, process, [process]() {
    if (process->state() != QProcess::NotRunning) {
      process->kill();
    }
  });
}

void InProcessDropExecutor::cancelActiveReceive(const std::string &sessionId)
{
  std::scoped_lock lock(m_mutex);
  if (!sessionId.empty() && m_activeReceiveSessionId == sessionId && m_receiverProcess &&
      m_receiverProcess->state() == QProcess::Running) {
    m_receiverProcess->write(QByteArray::fromStdString("CANCEL " + sessionId + "\n"));
  }
}

void InProcessDropExecutor::handleReceiveSessionStarted(
    size_t expectedFiles, const std::string &tokenTag, const std::string &sessionId
)
{
  finalizeActiveSession();
  bool shouldNotify = false;
  bool notifyActive = false;
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
    m_activeSessionCancelled = false;
    m_activeReceiveSessionId = sessionId;
    m_activeReceiveSession = true;
    notifyActiveTransfersChangedLocked(shouldNotify, notifyActive);
    if (!tokenTag.empty()) {
      auto it = std::find_if(
          m_pendingDropTargets.begin(), m_pendingDropTargets.end(),
          [&tokenTag](const DropTargetEntry &entry) { return entry.tokenTag == tokenTag; }
      );
      if (it != m_pendingDropTargets.end()) {
        m_activeSessionTarget = std::move(*it);
        m_pendingDropTargets.erase(it);
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
  if (shouldNotify) {
    emitActiveTransfersChanged(notifyActive);
  }
  std::function<void()> cancel;
  if (!sessionId.empty()) {
    cancel = [this, alive = m_alive, sessionId]() {
      std::scoped_lock lifetimeLock(alive->mutex);
      if (!alive->load()) {
        return;
      }
      if (alive->load()) {
        cancelActiveReceive(sessionId);
      }
    };
  }
  m_receiverOverlay.begin(
      QCoreApplication::translate("FileDropBridge", "Receiving %1 file(s)").arg(static_cast<qulonglong>(expectedFiles)),
      std::move(cancel)
  );
}

void InProcessDropExecutor::handleReceiveSessionFinished(bool cancelled, const std::string &sessionId)
{
  bool readyToFinalize = false;
  {
    std::scoped_lock lock(m_mutex);
    if (!sessionId.empty() && sessionId != m_activeReceiveSessionId) {
      return;
    }
    m_activeSessionEnded = true;
    m_activeSessionFailed = m_activeSessionFailed || cancelled;
    m_activeSessionCancelled = cancelled && !sessionId.empty();
    if (cancelled || m_activeSessionExpectedFiles == 0 ||
        m_activeSessionHandledFiles >= m_activeSessionExpectedFiles) {
      readyToFinalize = true;
    }
  }
  if (readyToFinalize) {
    finalizeActiveSession();
  }
}

void InProcessDropExecutor::handleReceiveFileFailedLine()
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

void InProcessDropExecutor::finalizeActiveSession()
{
  std::vector<std::string> filesForClipboard;
  bool shouldCopyToClipboard = false;
  bool sessionEnded = false;
  bool sessionFailed = false;
  bool sessionCancelled = false;
  std::string stagingDirStr;
  bool shouldNotify = false;
  bool notifyActive = false;
  {
    std::scoped_lock lock(m_mutex);
    if (m_activeSessionTarget.dir.empty() && m_activeSessionPlacedFiles.empty() && !m_activeSessionEnded) {
      return;
    }
    shouldCopyToClipboard = m_activeSessionTarget.copyToClipboard;
    sessionEnded = m_activeSessionEnded;
    sessionFailed = m_activeSessionFailed;
    sessionCancelled = m_activeSessionCancelled;
    if (shouldCopyToClipboard) {
      filesForClipboard = std::move(m_activeSessionPlacedFiles);
    }
    m_activeSessionPlacedFiles.clear();
    m_activeSessionTarget = DropTargetEntry{};
    m_activeReceiveSessionId.clear();
    m_activeSessionExpectedFiles = 0;
    m_activeSessionHandledFiles = 0;
    m_activeSessionEnded = false;
    m_activeSessionFailed = false;
    m_activeSessionCancelled = false;
    m_activeReceiveSession = false;
    notifyActiveTransfersChangedLocked(shouldNotify, notifyActive);
    stagingDirStr = m_stagingDir;
  }
  if (shouldNotify) {
    emitActiveTransfersChanged(notifyActive);
  }

  if (sessionCancelled) {
    m_receiverOverlay.finishCancelled();
  } else if (sessionEnded) {
    m_receiverOverlay.finish(
        !sessionFailed, sessionFailed
                            ? QCoreApplication::translate("FileDropBridge", "File receive failed or transfer was cancelled")
                            : QCoreApplication::translate("FileDropBridge", "File receive completed")
    );
  }

#if defined(_WIN32)
  if (shouldCopyToClipboard && !filesForClipboard.empty()) {
    copyFilesToWindowsClipboard(filesForClipboard);
  }
#else
  if (shouldCopyToClipboard && !filesForClipboard.empty()) {
    platform_drop::copyFilesToPlatformClipboard(filesForClipboard);
  }
#endif

  if (!stagingDirStr.empty()) {
    cleanEmptySubdirsInStaging(QString::fromStdString(stagingDirStr), true);
  }
}

void InProcessDropExecutor::handleReceivedFileLine(const std::string &filePath)
{
  std::string targetDir;
  std::string stagingDirStr;
  {
    std::scoped_lock lock(m_mutex);
    targetDir = m_activeSessionTarget.dir;
    stagingDirStr = m_stagingDir;
  }
  if (targetDir.empty()) {
    LOG_WARN("FileDropBridge: no authorized drop target for received file; keeping in staging");
    handleReceiveFileFailedLine();
    return;
  }

  QFileInfo srcInfo(QString::fromStdString(filePath));
  if (!srcInfo.isFile() || srcInfo.isSymLink()) {
    LOG_WARN("FileDropBridge: received path is missing or is not a regular file: \"%s\"", filePath.c_str());
    handleReceiveFileFailedLine();
    return;
  }

  QString relPath = srcInfo.fileName();
  if (!stagingDirStr.empty()) {
    QDir stagingDir(QString::fromStdString(stagingDirStr));
    const QString candidateRel = stagingDir.relativeFilePath(srcInfo.absoluteFilePath());
    if (!candidateRel.isEmpty() && candidateRel != QStringLiteral("..") &&
        !candidateRel.startsWith(QStringLiteral("../")) &&
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
    LOG_WARN("FileDropBridge: failed to move \"%s\" to \"%s\"", filePath.c_str(), dstPath.toStdString().c_str());
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

// --- OutOfProcessDropExecutor (Windows Service / Elevated interactive user session host) ---

#if defined(_WIN32)

bool isProcessRunningSystemOrElevated()
{
  HANDLE tok = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
    return false;
  }
  const bool isSystem = isLocalSystemToken(tok);
  TOKEN_ELEVATION elev = {};
  DWORD retLen = 0;
  const bool isElev = GetTokenInformation(tok, TokenElevation, &elev, sizeof(elev), &retLen) && elev.TokenIsElevated != 0;
  CloseHandle(tok);
  return isSystem || isElev;
}

bool shouldUseOutOfProcessDropAgent()
{
  if (const char *disable = std::getenv("DESKSEND_DISABLE_DROP_AGENT"); disable && std::strcmp(disable, "1") == 0) {
    return false;
  }
  if (const char *forceIn = std::getenv("DESKSEND_FORCE_INPROCESS_DROP_AGENT"); forceIn && std::strcmp(forceIn, "1") == 0) {
    return false;
  }
  auto *app = QCoreApplication::instance();
  if (!app) {
    return false;
  }
  if (const char *allowTest = std::getenv("DESKSEND_ALLOW_DROP_AGENT_FOR_TEST");
      !allowTest || std::strcmp(allowTest, "1") != 0) {
    const QString exeBase = QFileInfo(QCoreApplication::applicationFilePath()).completeBaseName();
    if (exeBase.compare(QStringLiteral("deskflow-core"), Qt::CaseInsensitive) != 0) {
      return false;
    }
  }
  return isProcessRunningSystemOrElevated();
}

struct OutOfProcessDropExecutor::PlatformState
{
  HANDLE hProcess = nullptr;
  HANDLE hStdinWrite = nullptr;
  HANDLE hStdoutRead = nullptr;
  std::thread readerThread;

  void closePipeAndProcess(std::mutex &writeMutex)
  {
    {
      std::scoped_lock wlock(writeMutex);
      if (hStdinWrite) {
        CloseHandle(hStdinWrite);
        hStdinWrite = nullptr;
      }
    }
    if (hProcess) {
      if (WaitForSingleObject(hProcess, 500) != WAIT_OBJECT_0) {
        TerminateProcess(hProcess, 0);
        WaitForSingleObject(hProcess, 1000);
      }
      CloseHandle(hProcess);
      hProcess = nullptr;
    }
    if (readerThread.joinable()) {
      if (readerThread.get_id() == std::this_thread::get_id()) {
        readerThread.detach();
      } else {
        const auto thread = static_cast<HANDLE>(readerThread.native_handle());
        while (WaitForSingleObject(thread, 20) == WAIT_TIMEOUT)
          CancelSynchronousIo(thread);
        readerThread.join();
      }
    }
    if (hStdoutRead) {
      CloseHandle(hStdoutRead);
      hStdoutRead = nullptr;
    }
  }
};

OutOfProcessDropExecutor::OutOfProcessDropExecutor() : m_platform(std::make_unique<PlatformState>())
{
}

OutOfProcessDropExecutor::~OutOfProcessDropExecutor()
{
  m_alive->store(false);
  std::scoped_lock launchLock(m_launchMutex);
  {
    std::scoped_lock lock(m_mutex);
    m_shuttingDown = true;
  }
  m_platform->closePipeAndProcess(m_writeMutex);
}

void OutOfProcessDropExecutor::setHost(IDropExecutorHost *host)
{
  m_host.store(host);
}

void OutOfProcessDropExecutor::execute(const DropAgentCommand &cmd)
{
  switch (cmd.type) {
  case DropAgentCommandType::UpdatePrimaryShape:
    {
      std::scoped_lock lock(m_mutex);
      m_hasPrimaryShape = true;
      m_shapeX = cmd.aw > 0 ? cmd.ax : cmd.x;
      m_shapeY = cmd.aw > 0 ? cmd.ay : cmd.y;
      m_shapeW = cmd.aw;
      m_shapeH = cmd.ah;
    }
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::ShowHover:
    m_shadowHoverIndicator.showHover(cmd.fileCount, cmd.x, cmd.y);
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::UpdateHoverCursor:
    m_shadowHoverIndicator.updateCursor(cmd.x, cmd.y);
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::HideHover:
    m_shadowHoverIndicator.hideHover();
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::BeginPreparing:
    m_shadowReceiverOverlay.beginPreparing(cmd.fileCount);
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::CancelPreparing:
    m_shadowReceiverOverlay.cancelPreparing();
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::TransferBegin:
    overlayFor(cmd.flag).begin(QString::fromStdString(cmd.text));
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::TransferProgress:
    overlayFor(cmd.flag).progress(cmd.done, cmd.total);
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::TransferFinish:
    {
      std::scoped_lock lock(m_mutex);
      m_dragActive = cmd.dragActive;
    }
    overlayFor(cmd.flag).finish(cmd.success, QString::fromStdString(cmd.text));
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::StartReceiver:
    {
      std::scoped_lock lock(m_mutex);
      m_receiverDesired = !cmd.items.empty();
      m_receiverHosts = ReceiverRestartPolicy::normalizeHosts(cmd.items);
      if (!m_receiverDesired) {
        m_cachedAuthLines.clear();
      }
    }
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::StopReceiver:
    {
      std::scoped_lock lock(m_mutex);
      m_receiverDesired = false;
      m_receiverHosts.clear();
      m_cachedAuthLines.clear();
    }
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::ReceiverAuthCommand:
    {
      std::scoped_lock lock(m_mutex);
      if (m_cachedAuthLines.size() >= 64) {
        m_cachedAuthLines.pop_front();
      }
      m_cachedAuthLines.push_back(cmd.text);
    }
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::SetDragActive:
    {
      std::scoped_lock lock(m_mutex);
      m_dragActive = cmd.dragActive;
    }
    sendOrQueueCommand(cmd);
    return;
  case DropAgentCommandType::Shutdown:
    {
      std::scoped_lock launchLock(m_launchMutex);
      {
        std::scoped_lock lock(m_mutex);
        m_shuttingDown = true;
      }
      m_platform->closePipeAndProcess(m_writeMutex);
    }
    return;
  default:
    sendOrQueueCommand(cmd);
    return;
  }
}

bool OutOfProcessDropExecutor::isHoverVisible() const
{
  return m_shadowHoverIndicator.isVisible();
}

uint32_t OutOfProcessDropExecutor::hoverFileCount() const
{
  return m_shadowHoverIndicator.fileCount();
}

QPoint OutOfProcessDropExecutor::hoverCursorPos() const
{
  return m_shadowHoverIndicator.cursorPos();
}

TransferStatusOverlay::Phase OutOfProcessDropExecutor::receiverOverlayPhase() const
{
  return m_shadowReceiverOverlay.phase();
}

uint32_t OutOfProcessDropExecutor::receiverPreparingFileCount() const
{
  return m_shadowReceiverOverlay.preparingFileCount();
}

TransferStatusOverlay &OutOfProcessDropExecutor::overlayFor(bool receiving)
{
  return receiving ? m_shadowReceiverOverlay : m_shadowSenderOverlay;
}

bool OutOfProcessDropExecutor::hasActiveTransfers() const
{
  std::scoped_lock lock(m_mutex);
  return m_activeTransfers;
}

void OutOfProcessDropExecutor::writeCommandLocked(const DropAgentCommand &cmd)
{
  if (!m_platform->hStdinWrite) {
    return;
  }
  std::string line = serializeDropAgentCommand(cmd);
  if (line.size() > kMaxDropAgentLineBytes)
    return;
  line.push_back('\n');
  OVERLAPPED operation{};
  operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (!operation.hEvent)
    return;
  DWORD written = 0;
  const auto length = static_cast<DWORD>(line.size());
  bool success = WriteFile(m_platform->hStdinWrite, line.data(), length, &written, &operation);
  if (!success && GetLastError() == ERROR_IO_PENDING) {
    if (WaitForSingleObject(operation.hEvent, 250) == WAIT_OBJECT_0) {
      success = GetOverlappedResult(m_platform->hStdinWrite, &operation, &written, FALSE);
    } else {
      CancelIoEx(m_platform->hStdinWrite, &operation);
      GetOverlappedResult(m_platform->hStdinWrite, &operation, &written, TRUE);
    }
  }
  CloseHandle(operation.hEvent);
  if (!success || written != length) {
    // 未完整的帧不能与后续命令拼接。
    CloseHandle(m_platform->hStdinWrite);
    m_platform->hStdinWrite = nullptr;
    if (m_platform->hProcess)
      TerminateProcess(m_platform->hProcess, 1);
  }
}

void OutOfProcessDropExecutor::sendOrQueueCommand(const DropAgentCommand &cmd)
{
  ensureAgentRunning();

  bool canWriteNow = false;
  {
    std::scoped_lock lock(m_mutex);
    if (m_shuttingDown) {
      return;
    }
    if (m_agentReady) {
      canWriteNow = true;
    } else {
      // Transient per-frame moves/hover updates don't need queuing while agent is booting or desktop locked.
      const bool transient = cmd.type == DropAgentCommandType::PrimaryMouseMove ||
                             cmd.type == DropAgentCommandType::UpdateHoverCursor;
      if (!transient) {
        if (m_startupQueue.size() >= 128) {
          m_startupQueue.pop_front();
        }
        m_startupQueue.push_back(cmd);
      }
    }
  }
  if (canWriteNow) {
    std::scoped_lock wlock(m_writeMutex);
    writeCommandLocked(cmd);
  }
}

void OutOfProcessDropExecutor::ensureAgentRunning()
{
  std::unique_lock launchLock(m_launchMutex);
  {
    std::scoped_lock lock(m_mutex);
    if (m_shuttingDown) {
      return;
    }
  }
  if (m_platform->hProcess != nullptr && WaitForSingleObject(m_platform->hProcess, 0) == WAIT_TIMEOUT) {
    return;
  }
  if (m_platform->hProcess != nullptr || m_platform->readerThread.joinable()) {
    m_platform->closePipeAndProcess(m_writeMutex);
    std::scoped_lock lock(m_mutex);
    m_agentReady = false;
  }

  QString agentExe;
  if (const char *overrideExe = std::getenv("DESKSEND_DROP_AGENT_EXE"); overrideExe && overrideExe[0] != '\0') {
    agentExe = QString::fromUtf8(overrideExe);
  } else {
    agentExe = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("deskflow-core.exe"));
  }
  if (!QFileInfo::exists(agentExe)) {
    LOG_WARN("FileDropBridge: drop-agent executable not found at \"%s\"", agentExe.toStdString().c_str());
    return;
  }

  HANDLE userToken = nullptr;
  bool useTokenFallback = false;
  if (!acquireNonSystemUserPrimaryToken(userToken, useTokenFallback)) {
    // Lock screen or no active user session yet: retry non-blockingly without halting input sharing.
    int delay = 1000;
    {
      std::scoped_lock lock(m_mutex);
      delay = std::min(5000, 1000 * (1 << std::min(m_restartAttempt, 3)));
      ++m_restartAttempt;
    }
    launchLock.unlock();
    scheduleReconnect(delay);
    return;
  }

  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE childStdinRead = nullptr;
  HANDLE parentStdinWrite = nullptr;
  HANDLE parentStdoutRead = nullptr;
  HANDLE childStdoutWrite = nullptr;
  HANDLE childStderr = INVALID_HANDLE_VALUE;

  auto cleanupPipes = [&]() {
    if (childStdinRead)
      CloseHandle(childStdinRead);
    if (parentStdinWrite)
      CloseHandle(parentStdinWrite);
    if (parentStdoutRead)
      CloseHandle(parentStdoutRead);
    if (childStdoutWrite)
      CloseHandle(childStdoutWrite);
    if (childStderr != INVALID_HANDLE_VALUE)
      CloseHandle(childStderr);
  };

  const auto pipeName = (QStringLiteral(R"(\\.\pipe\DeskSendDrop-)") +
                         QUuid::createUuid().toString(QUuid::Id128)).toStdWString();
  parentStdinWrite = CreateNamedPipeW(
      pipeName.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
      PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 0, 0, nullptr
  );
  if (parentStdinWrite != INVALID_HANDLE_VALUE) {
    childStdinRead = CreateFileW(pipeName.c_str(), GENERIC_READ, 0, &sa, OPEN_EXISTING, 0, nullptr);
    if (childStdinRead == INVALID_HANDLE_VALUE)
      childStdinRead = nullptr;
  } else {
    parentStdinWrite = nullptr;
  }
  if (!childStdinRead || !CreatePipe(&parentStdoutRead, &childStdoutWrite, &sa, 0)) {
    CloseHandle(userToken);
    cleanupPipes();
    return;
  }
  SetHandleInformation(parentStdinWrite, HANDLE_FLAG_INHERIT, 0);
  SetHandleInformation(parentStdoutRead, HANDLE_FLAG_INHERIT, 0);
  childStderr = CreateFileW(
      L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr
  );

  LPVOID envBlock = nullptr;
  CreateEnvironmentBlock(&envBlock, userToken, FALSE);

  std::wstring exeW = QDir::toNativeSeparators(agentExe).toStdWString();
  std::wstring workDirW = QDir::toNativeSeparators(QFileInfo(agentExe).absolutePath()).toStdWString();
  std::wstring cmdLineW = L"\"" + exeW + L"\" --drop-agent";
  if (!Settings::isPortableMode()) {
    const QString settingsFile = Settings::settingsFile();
    if (!settingsFile.isEmpty()) {
      QString escapedSettings = QDir::toNativeSeparators(settingsFile);
      escapedSettings.replace(QLatin1Char('"'), QStringLiteral("\\\""));
      cmdLineW += L" --settings \"" + escapedSettings.toStdWString() + L"\"";
    }
  }
  wchar_t desktopName[] = L"winsta0\\default";

  STARTUPINFOW si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  si.lpDesktop = desktopName;
  si.hStdInput = childStdinRead;
  si.hStdOutput = childStdoutWrite;
  si.hStdError = (childStderr != INVALID_HANDLE_VALUE) ? childStderr : nullptr;

  PROCESS_INFORMATION pi = {};
  const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW;
  BOOL launched = FALSE;
  if (!useTokenFallback) {
    launched = CreateProcessAsUserW(
        userToken, exeW.c_str(), cmdLineW.data(), nullptr, nullptr, TRUE, flags, envBlock, workDirW.c_str(), &si, &pi
    );
  } else {
    launched = CreateProcessWithTokenW(
        userToken, LOGON_WITH_PROFILE, exeW.c_str(), cmdLineW.data(), flags, envBlock, workDirW.c_str(), &si, &pi
    );
  }

  const DWORD lastErr = launched ? 0 : GetLastError();
  if (envBlock) {
    DestroyEnvironmentBlock(envBlock);
  }
  CloseHandle(userToken);
  CloseHandle(childStdinRead);
  childStdinRead = nullptr;
  CloseHandle(childStdoutWrite);
  childStdoutWrite = nullptr;
  if (childStderr != INVALID_HANDLE_VALUE) {
    CloseHandle(childStderr);
    childStderr = INVALID_HANDLE_VALUE;
  }

  if (!launched) {
    CloseHandle(parentStdinWrite);
    CloseHandle(parentStdoutRead);
    LOG_WARN("FileDropBridge: failed to spawn user-session drop-agent (err=%lu)", lastErr);
    int delay = 1000;
    {
      std::scoped_lock lock(m_mutex);
      delay = std::min(5000, 1000 * (1 << std::min(m_restartAttempt, 3)));
      ++m_restartAttempt;
    }
    launchLock.unlock();
    scheduleReconnect(delay);
    return;
  }

  assignProcessHandleToKillOnCloseJob(pi.hProcess);
  CloseHandle(pi.hThread);

  uint32_t sessionGen = 0;
  {
    std::scoped_lock lock(m_mutex);
    std::scoped_lock wlock(m_writeMutex);
    m_platform->hProcess = pi.hProcess;
    m_platform->hStdinWrite = parentStdinWrite;
    m_platform->hStdoutRead = parentStdoutRead;
    m_agentReady = false;
    m_reconnectScheduled = false;
    sessionGen = ++m_sessionGeneration;
  }

  m_platform->readerThread = std::thread([this, sessionGen]() { readerLoop(sessionGen); });
  LOG_INFO("FileDropBridge: launched interactive user-session drop-agent (pid=%lu)", pi.dwProcessId);
}

void OutOfProcessDropExecutor::scheduleReconnect(int delayMs)
{
  {
    std::scoped_lock lock(m_mutex);
    if (m_shuttingDown || m_reconnectScheduled) {
      return;
    }
    m_reconnectScheduled = true;
  }
  if (auto *app = QCoreApplication::instance()) {
    const auto alive = m_alive;
    QMetaObject::invokeMethod(
        app,
        [this, alive, delayMs]() {
          if (!alive->load()) {
            return;
          }
          QTimer::singleShot(delayMs, QCoreApplication::instance(), [this, alive]() {
            if (!alive->load()) {
              return;
            }
            {
              std::scoped_lock lock(m_mutex);
              m_reconnectScheduled = false;
              if (m_shuttingDown) {
                return;
              }
            }
            ensureAgentRunning();
          });
        },
        Qt::QueuedConnection
    );
  }
}

void OutOfProcessDropExecutor::postToMainThread(std::function<void()> fn)
{
  if (!m_alive->load()) {
    return;
  }
  if (auto *app = QCoreApplication::instance()) {
    if (QThread::currentThread() == app->thread()) {
      fn();
    } else {
      const auto alive = m_alive;
      QMetaObject::invokeMethod(
          app,
          [alive, fn = std::move(fn)]() {
            if (alive->load()) {
              fn();
            }
          },
          Qt::QueuedConnection
      );
    }
  }
}

void OutOfProcessDropExecutor::readerLoop(uint32_t expectedGeneration)
{
  HANDLE readHandle = m_platform->hStdoutRead;
  std::string buffer;
  std::array<char, 4096> chunk{};
  while (readHandle != nullptr) {
    DWORD bytesRead = 0;
    const BOOL ok = ReadFile(readHandle, chunk.data(), static_cast<DWORD>(chunk.size()), &bytesRead, nullptr);
    if (!ok || bytesRead == 0) {
      break;
    }
    buffer.append(chunk.data(), bytesRead);
    size_t pos = 0;
    bool oversized = false;
    while (true) {
      const size_t nl = buffer.find('\n', pos);
      if (nl == std::string::npos) {
        break;
      }
      if (nl - pos > kMaxDropAgentLineBytes) {
        oversized = true;
        break;
      }
      std::string line = buffer.substr(pos, nl - pos);
      pos = nl + 1;
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      if (!line.empty() && line.front() == '{') {
        if (auto ev = deserializeDropAgentEvent(line); ev.has_value()) {
          handleAgentEvent(*ev);
        }
      }
    }
    if (pos > 0)
      buffer.erase(0, pos);
    if (oversized || buffer.size() > kMaxDropAgentLineBytes)
      break;
  }

  postToMainThread([this, expectedGeneration]() { handleAgentPipeClosed(expectedGeneration); });
}

void OutOfProcessDropExecutor::handleAgentPipeClosed(uint32_t expectedGeneration)
{
  bool activeChanged = false;
  bool shouldRestart = false;
  int delayMs = 1000;
  {
    std::scoped_lock launchLock(m_launchMutex);
    {
      std::scoped_lock lock(m_mutex);
      if (m_shuttingDown || expectedGeneration != m_sessionGeneration) {
        return;
      }
    }
    m_platform->closePipeAndProcess(m_writeMutex);
    {
      std::scoped_lock lock(m_mutex);
      m_agentReady = false;
      if (m_activeTransfers) {
        m_activeTransfers = false;
        activeChanged = true;
      }
      shouldRestart = !m_shuttingDown;
      if (shouldRestart) {
        delayMs = std::min(5000, 1000 * (1 << std::min(m_restartAttempt, 3)));
        ++m_restartAttempt;
      }
    }
  }

  if (activeChanged) {
    ipcSendToClient(QStringLiteral("activeTransfers"), QStringLiteral("0"));
  }
  if (auto *host = m_host.load()) {
    if (activeChanged) {
      host->onActiveTransfersChanged(false);
    }
    host->onPendingDragCleared();
  }
  if (shouldRestart) {
    LOG_WARN("FileDropBridge: drop-agent pipe closed; scheduling reconnect in %d ms", delayMs);
    scheduleReconnect(delayMs);
  }
}

void OutOfProcessDropExecutor::replayCachedStateLocked()
{
  if (m_hasPrimaryShape) {
    DropAgentCommand shapeCmd;
    shapeCmd.type = DropAgentCommandType::UpdatePrimaryShape;
    shapeCmd.x = m_shapeX;
    shapeCmd.y = m_shapeY;
    shapeCmd.ax = m_shapeX;
    shapeCmd.ay = m_shapeY;
    shapeCmd.aw = m_shapeW;
    shapeCmd.ah = m_shapeH;
    writeCommandLocked(shapeCmd);
  }
  if (m_dragActive) {
    DropAgentCommand activeCmd;
    activeCmd.type = DropAgentCommandType::SetDragActive;
    activeCmd.dragActive = true;
    writeCommandLocked(activeCmd);
  }
  if (m_receiverDesired && !m_receiverHosts.empty()) {
    DropAgentCommand rxCmd;
    rxCmd.type = DropAgentCommandType::StartReceiver;
    rxCmd.items = m_receiverHosts;
    writeCommandLocked(rxCmd);
    for (const auto &authLine : m_cachedAuthLines) {
      DropAgentCommand authCmd;
      authCmd.type = DropAgentCommandType::ReceiverAuthCommand;
      authCmd.text = authLine;
      writeCommandLocked(authCmd);
    }
  }
  while (!m_startupQueue.empty()) {
    const auto cmd = std::move(m_startupQueue.front());
    m_startupQueue.pop_front();
    // Skip duplicate shape/startReceiver/auth commands if already replayed cleanly.
    if (cmd.type != DropAgentCommandType::UpdatePrimaryShape && cmd.type != DropAgentCommandType::StartReceiver &&
        cmd.type != DropAgentCommandType::ReceiverAuthCommand) {
      writeCommandLocked(cmd);
    }
  }
}

void OutOfProcessDropExecutor::handleAgentEvent(const DropAgentEvent &ev)
{
  switch (ev.type) {
  case DropAgentEventType::Ready:
    {
      std::scoped_lock lock(m_mutex);
      std::scoped_lock wlock(m_writeMutex);
      m_agentReady = true;
      m_restartAttempt = 0;
      replayCachedStateLocked();
    }
    break;
  case DropAgentEventType::ActiveTransfersChanged:
    {
      std::scoped_lock lock(m_mutex);
      m_activeTransfers = ev.flag;
    }
    break;
  default:
    break;
  }

  postToMainThread([this, ev]() { dispatchHostEvent(ev); });
}

void OutOfProcessDropExecutor::dispatchHostEvent(const DropAgentEvent &ev)
{
  IDropExecutorHost *host = m_host.load();
  switch (ev.type) {
  case DropAgentEventType::Ready:
    if (host) {
      host->onExecutorReady();
    }
    break;
  case DropAgentEventType::PendingDragCaptured:
    if (host) {
      host->onPendingDragCaptured(ev.files, ev.sideIndex, ev.x, ev.y);
    }
    break;
  case DropAgentEventType::DragCancellationCompleted:
    if (host) {
      host->onDragCancellationCompleted(ev.flag);
    }
    break;
  case DropAgentEventType::PendingDragCleared:
    if (host) {
      host->onPendingDragCleared();
    }
    break;
  case DropAgentEventType::ActiveTransfersChanged:
    ipcSendToClient(QStringLiteral("activeTransfers"), ev.flag ? QStringLiteral("1") : QStringLiteral("0"));
    if (host) {
      host->onActiveTransfersChanged(ev.flag);
    }
    break;
  case DropAgentEventType::SendCompleted:
    if (host) {
      host->onSendCompleted(ev.text, ev.flag);
    }
    break;
  case DropAgentEventType::LogMessage:
    if (host) {
      host->onExecutorLog(ev.level, ev.text);
    } else {
      LOG_INFO("FileDropBridge [agent]: %s", ev.text.c_str());
    }
    break;
  }
}

#endif // _WIN32

std::unique_ptr<IDropExecutor> createDefaultDropExecutor()
{
#if defined(_WIN32)
  if (shouldUseOutOfProcessDropAgent()) {
    return std::make_unique<OutOfProcessDropExecutor>();
  }
#endif
  return std::make_unique<InProcessDropExecutor>(true);
}

} // namespace deskflow
