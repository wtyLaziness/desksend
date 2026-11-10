// Phase 0: 边缘哨兵窗口可行性验证
// 在屏幕边缘创建近乎透明的置顶窗口并注册 IDropTarget,
// 当用户从资源管理器拖拽文件经过时,打印 CF_HDROP 中的文件路径。
// 用法: sentinel.exe [left|right] [宽度px]   默认 right 6
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <ole2.h>
#include <cstdarg>
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

// 读取 IDataObject 中的文件清单,返回文件数
static int dumpFiles(IDataObject *obj)
{
  FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM med = {};
  HRESULT hr = obj->GetData(&fmt, &med);
  if (FAILED(hr)) {
    logf(L"  GetData(CF_HDROP) 失败 hr=0x%08lx (拖拽的可能不是文件)", (unsigned long)hr);
    return 0;
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
