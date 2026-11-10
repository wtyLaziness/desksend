// Phase 0: 边缘哨兵窗口可行性验证
// 在屏幕边缘创建近乎透明的置顶窗口并注册 IDropTarget,
// 当用户从资源管理器拖拽文件经过时,打印 CF_HDROP 中的文件路径。
// 用法: sentinel.exe [left|right] [宽度px]   默认 right 6
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <ole2.h>
#include <cstdarg>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <string>

static FILE *g_log = nullptr;

static void logf(const wchar_t *fmt, ...)
{
  SYSTEMTIME t;
  GetLocalTime(&t);
  wchar_t body[2048];
  va_list ap;
  va_start(ap, fmt);
  vswprintf(body, 2048, fmt, ap);
  va_end(ap);
  wchar_t line[2200];
  swprintf(line, 2200, L"[%02d:%02d:%02d.%03d] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, body);
  fputws(line, stdout);
  fflush(stdout);
  if (g_log) {
    fputws(line, g_log);
    fflush(g_log);
  }
}

// 列出数据对象支持的剪贴板格式(诊断用)
static void listFormats(IDataObject *obj)
{
  IEnumFORMATETC *en = nullptr;
  if (FAILED(obj->EnumFormatEtc(DATADIR_GET, &en)) || !en) {
    logf(L"  EnumFormatEtc 失败");
    return;
  }
  FORMATETC f;
  while (en->Next(1, &f, nullptr) == S_OK) {
    wchar_t name[128] = L"";
    if (f.cfFormat >= 0xC000)
      GetClipboardFormatNameW(f.cfFormat, name, 128);
    logf(L"  format cf=%u '%s' tymed=0x%lx", (unsigned)f.cfFormat, name, (unsigned long)f.tymed);
  }
  en->Release();
}

// 备用路径: Shell IDList Array(CFSTR_SHELLIDLIST)
static int dumpViaShellIdList(IDataObject *obj)
{
  FORMATETC fmt = {(CLIPFORMAT)RegisterClipboardFormatW(CFSTR_SHELLIDLIST), nullptr, DVASPECT_CONTENT, -1,
                   TYMED_HGLOBAL};
  STGMEDIUM med = {};
  if (FAILED(obj->GetData(&fmt, &med)))
    return 0;
  CIDA *cida = static_cast<CIDA *>(GlobalLock(med.hGlobal));
  int n = 0;
  PCUIDLIST_ABSOLUTE parent = reinterpret_cast<PCUIDLIST_ABSOLUTE>(reinterpret_cast<BYTE *>(cida) + cida->aoffset[0]);
  for (UINT i = 0; i < cida->cidl; ++i) {
    PCUIDLIST_RELATIVE child =
        reinterpret_cast<PCUIDLIST_RELATIVE>(reinterpret_cast<BYTE *>(cida) + cida->aoffset[i + 1]);
    PIDLIST_ABSOLUTE full = ILCombine(parent, child);
    wchar_t path[MAX_PATH * 2];
    if (full && SHGetPathFromIDListW(full, path)) {
      logf(L"  [idlist %u] %s", i, path);
      ++n;
    }
    if (full)
      ILFree(full);
  }
  GlobalUnlock(med.hGlobal);
  ReleaseStgMedium(&med);
  return n;
}

// 读取 IDataObject 中的文件清单,返回文件数
static int dumpFiles(IDataObject *obj)
{
  FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM med = {};
  HRESULT hr = obj->GetData(&fmt, &med);
  if (FAILED(hr)) {
    logf(L"  GetData(CF_HDROP) failed hr=0x%08lx, trying SHELLIDLIST", (unsigned long)hr);
    listFormats(obj);
    return dumpViaShellIdList(obj);
  }
  HDROP drop = static_cast<HDROP>(GlobalLock(med.hGlobal));
  UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
  for (UINT i = 0; i < n; ++i) {
    wchar_t path[MAX_PATH * 2];
    DragQueryFileW(drop, i, path, MAX_PATH * 2);
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    unsigned long long size = 0;
    bool dir = false;
    if (GetFileAttributesExW(path, GetFileExInfoStandard, &a)) {
      size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
      dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    logf(L"  [%u] %s %s size=%llu", i, dir ? L"<DIR>" : L"     ", path, size);
  }
  GlobalUnlock(med.hGlobal);
  ReleaseStgMedium(&med);
  return (int)n;
}

class DropTarget : public IDropTarget
{
public:
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
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
    logf(L"DragEnter at (%ld,%ld)", pt.x, pt.y);
    dumpFiles(obj);
    *eff = DROPEFFECT_COPY;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD *eff) override
  {
    *eff = DROPEFFECT_COPY;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DragLeave() override
  {
    logf(L"DragLeave");
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Drop(IDataObject *obj, DWORD, POINTL pt, DWORD *eff) override
  {
    logf(L"Drop at (%ld,%ld)", pt.x, pt.y);
    dumpFiles(obj);
    *eff = DROPEFFECT_COPY;
    return S_OK;
  }
};

// 仅含 CF_HDROP 的最小 IDataObject,用于自检
class MemDataObject : public IDataObject
{
public:
  explicit MemDataObject(HGLOBAL hg) : m_hg(hg) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
  {
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *ppv = static_cast<IDataObject *>(this);
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *f, STGMEDIUM *m) override
  {
    if (f->cfFormat != CF_HDROP || !(f->tymed & TYMED_HGLOBAL))
      return DV_E_FORMATETC;
    SIZE_T sz = GlobalSize(m_hg);
    HGLOBAL copy = GlobalAlloc(GHND, sz);
    memcpy(GlobalLock(copy), GlobalLock(m_hg), sz);
    GlobalUnlock(copy);
    GlobalUnlock(m_hg);
    m->tymed = TYMED_HGLOBAL;
    m->hGlobal = copy;
    m->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *f) override
  {
    return f->cfFormat == CF_HDROP ? S_OK : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC **e) override
  {
    FORMATETC f = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return SHCreateStdEnumFmtEtc(1, &f, e);
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
  HGLOBAL m_hg;
};

// 自检: 不依赖真实鼠标输入,验证
// 1) RegisterDragDrop 成功且 OLE 已记录该窗口的 DropTarget
// 2) 窗口在 alpha=1 时仍能被命中测试(WindowFromPoint)
// 3) 用 Shell 生成的 IDataObject(与资源管理器拖拽同类型)能被正确解析出文件清单
static int runSelfTest(HWND hwnd, DropTarget &target, HRESULT regHr, int x, int width, int height)
{
  int fails = 0;
  auto check = [&](bool ok, const wchar_t *what) {
    logf(L"[%s] %s", ok ? L"PASS" : L"FAIL", what);
    if (!ok)
      ++fails;
  };
  check(SUCCEEDED(regHr), L"RegisterDragDrop 成功");
  check(GetPropW(hwnd, L"OleDropTargetInterface") != nullptr, L"OLE 已记录窗口的 DropTarget");

  // 窗口需要先处理一次消息才可被命中
  MSG msg;
  for (int i = 0; i < 20; ++i) {
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
      DispatchMessageW(&msg);
    Sleep(20);
  }
  POINT pt = {x + width / 2, height / 2};
  HWND hit = WindowFromPoint(pt);
  check(hit == hwnd, L"边缘点命中哨兵窗口(WindowFromPoint)");

  // 构造测试文件
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring dir = std::wstring(tmp) + L"dd_selftest";
  CreateDirectoryW(dir.c_str(), nullptr);
  std::wstring f1 = dir + L"\\one.txt", f2 = dir + L"\\two.txt", sub = dir + L"\\folder";
  for (auto &f : {f1, f2}) {
    HANDLE h = CreateFileW(f.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD w;
    WriteFile(h, "hello", 5, &w, nullptr);
    CloseHandle(h);
  }
  CreateDirectoryW(sub.c_str(), nullptr);

  PIDLIST_ABSOLUTE pidls[3] = {};
  const std::wstring *paths[3] = {&f1, &f2, &sub};
  bool pidlOk = true;
  for (int i = 0; i < 3; ++i)
    pidlOk &= SUCCEEDED(SHParseDisplayName(paths[i]->c_str(), nullptr, &pidls[i], 0, nullptr));
  check(pidlOk, L"构造 3 个 PIDL");

  // 测试 A: Shell 数据对象(规范用法: 父目录 PIDL + 子项 PIDL)
  PIDLIST_ABSOLUTE parentPidl = nullptr;
  SHParseDisplayName(dir.c_str(), nullptr, &parentPidl, 0, nullptr);
  PCUITEMID_CHILD children[3] = {};
  for (int i = 0; i < 3; ++i)
    children[i] = ILFindLastID(pidls[i]);
  IDataObject *obj = nullptr;
  HRESULT hr = SHCreateDataObject(parentPidl, 3, children, nullptr, IID_IDataObject, (void **)&obj);
  check(SUCCEEDED(hr) && obj, L"A: SHCreateDataObject(parent+children)");
  if (obj) {
    POINTL p = {pt.x, pt.y};
    DWORD eff = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    check(target.DragEnter(obj, MK_LBUTTON, p, &eff) == S_OK && eff == DROPEFFECT_COPY, L"A: DragEnter 返回 COPY");
    int n = dumpFiles(obj);
    check(n == 3, L"A: Shell 数据对象解析出 3 项");
    check(target.Drop(obj, MK_LBUTTON, p, &eff) == S_OK, L"A: Drop 返回 S_OK");
    obj->Release();
  }

  // 测试 B: 自建 CF_HDROP 数据对象(仅验证解析逻辑)
  {
    std::wstring list;
    for (auto *s : paths) {
      list += *s;
      list.push_back(L'\0');
    }
    list.push_back(L'\0');
    size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
    HGLOBAL hg = GlobalAlloc(GHND, bytes);
    DROPFILES *df = static_cast<DROPFILES *>(GlobalLock(hg));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy(reinterpret_cast<BYTE *>(df) + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
    GlobalUnlock(hg);
    MemDataObject mem(hg);
    int n = dumpFiles(&mem);
    check(n == 3, L"B: 自建 CF_HDROP 解析出 3 项");
  }
  CoTaskMemFree(parentPidl);
  for (auto &p : pidls)
    if (p)
      CoTaskMemFree(p);

  RevokeDragDrop(hwnd);
  DestroyWindow(hwnd);
  OleUninitialize();
  logf(L"自检结果: %s (失败 %d 项)", fails ? L"FAIL" : L"ALL PASS", fails);
  return fails ? 1 : 0;
}

static BOOL WINAPI onCtrl(DWORD)
{
  PostQuitMessage(0);
  return TRUE;
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  if (m == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

int wmain(int argc, wchar_t **argv)
{
  bool left = argc > 1 && std::wstring(argv[1]) == L"left";
  int width = argc > 2 ? _wtoi(argv[2]) : 6;
  if (width < 1)
    width = 6;

  _setmode(_fileno(stdout), _O_U8TEXT);
  _wfopen_s(&g_log, L"sentinel.log", L"w, ccs=UTF-8");
  SetProcessDPIAware();
  if (FAILED(OleInitialize(nullptr))) {
    logf(L"OleInitialize 失败");
    return 1;
  }

  WNDCLASSW wc = {};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"DesksendSentinel";
  RegisterClassW(&wc);

  int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
  int x = left ? 0 : sw - width;
  HWND hwnd = CreateWindowExW(
      WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE, wc.lpszClassName, L"sentinel", WS_POPUP, x,
      0, width, sh, nullptr, nullptr, wc.hInstance, nullptr
  );
  // alpha=1: 视觉上几乎不可见,但仍参与鼠标命中测试(alpha=0 会穿透)
  SetLayeredWindowAttributes(hwnd, 0, 1, LWA_ALPHA);
  ShowWindow(hwnd, SW_SHOWNOACTIVATE);

  DropTarget target;
  HRESULT hr = RegisterDragDrop(hwnd, &target);

  if (argc > 1 && std::wstring(argv[1]) == L"--selftest")
    return runSelfTest(hwnd, target, hr, x, width, sh);

  logf(L"哨兵窗口: %s 边缘, x=%d w=%d h=%d, RegisterDragDrop hr=0x%08lx", left ? L"左" : L"右", x, width, sh,
       (unsigned long)hr);
  logf(L"请在资源管理器中选中文件并拖到该边缘; Ctrl+C 退出");

  SetConsoleCtrlHandler(onCtrl, TRUE);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  RevokeDragDrop(hwnd);
  DestroyWindow(hwnd);
  OleUninitialize();
  return 0;
}
