// The GPU check's window (--gui: the Start menu's and the desktop's shortcut): a row per API with its verdict, filled
// in as each group ends (the tests run on a thread of their own, each group in its child process as always), and the
// full report a click away. Plain Win32 and GDI, painted rather than built of common controls, so it looks the same
// from Windows 7 to 11 without a manifest. A row's click lists what didn't pass in it.

#include <mutex>

static std::mutex g_guiLock;
static std::vector<Result> g_guiResults;          // under g_guiLock, like the three below
static std::vector<const Group *> g_guiGroups;
static size_t g_guiDone = 0;                      // groups finished
static std::string g_guiReport;                   // the HTML report, once complete
static HWND g_guiWnd;

static std::wstring wide(const std::string &s)
{
   std::wstring w(s.size(), L'\0');
   w.resize(MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], (int)w.size()));
   return w;
}

static void guiProgress(const std::vector<Result> &results, const std::vector<const Group *> &groups, size_t done,
                        const std::string &report)
{
   {
      std::lock_guard<std::mutex> lock(g_guiLock);
      g_guiResults = results;
      g_guiGroups = groups;
      g_guiDone = done;
      if (done == groups.size()) g_guiReport = report;
   }
   InvalidateRect(g_guiWnd, nullptr, FALSE);
}

static int g_guiDpi = 96;
static int px(int v) { return MulDiv(v, g_guiDpi, 96); }

// Layout (96 DPI units): the title, a status line, the rows, two buttons at the bottom
static const int ROW_TOP = 88, ROW_H = 32, MARGIN = 24, BUTTON_W = 150, BUTTON_H = 30;

static int guiRowAt(int y, size_t rows)
{
   if (y < px(ROW_TOP)) return -1;
   size_t row = (size_t)((y - px(ROW_TOP)) / px(ROW_H));
   return row < rows ? (int)row : -1;
}

static RECT guiButton(HWND wnd, int which) // 0: the full report, 1: close
{
   RECT client;
   GetClientRect(wnd, &client);
   int right = client.right - px(MARGIN) - which * px(BUTTON_W + 12);
   return {right - px(BUTTON_W), client.bottom - px(MARGIN + BUTTON_H), right, client.bottom - px(MARGIN)};
}

static void guiPaint(HWND wnd, HDC dc)
{
   RECT client;
   GetClientRect(wnd, &client);
   HBRUSH back = CreateSolidBrush(RGB(250, 250, 250));
   FillRect(dc, &client, back);
   DeleteObject(back);
   SetBkMode(dc, TRANSPARENT);

   NONCLIENTMETRICSW metrics = {};
   metrics.cbSize = sizeof(metrics);
   SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
   LOGFONTW lf = metrics.lfMessageFont;
   lf.lfHeight = -px(15);
   HFONT text = CreateFontIndirectW(&lf);
   lf.lfHeight = -px(22);
   lf.lfWeight = FW_SEMIBOLD;
   HFONT title = CreateFontIndirectW(&lf);
   lf.lfHeight = -px(15);
   HFONT bold = CreateFontIndirectW(&lf);

   std::lock_guard<std::mutex> lock(g_guiLock);
   auto total = countResults(g_guiResults, nullptr);
   bool complete = !g_guiGroups.empty() && g_guiDone == g_guiGroups.size();

   SelectObject(dc, title);
   SetTextColor(dc, RGB(32, 32, 32));
   TextOutW(dc, px(MARGIN), px(20), L"WinBoat GPU Check", 17);
   SelectObject(dc, text);
   SetTextColor(dc, RGB(96, 96, 96));
   std::string status = complete ? fmt("Done: %d checks passed, %d failed, %d optional features not supported, %d skipped",
                                       total[0], total[2], total[1], total[4])
                        : g_guiDone < g_guiGroups.size() ? "Testing " + std::string(g_guiGroups[g_guiDone]->title) + "..."
                                                         : std::string("Starting...");
   std::wstring s = wide(status);
   TextOutW(dc, px(MARGIN), px(54), s.c_str(), (int)s.size());

   for (size_t g = 0; g < g_guiGroups.size(); g++) {
      int top = px(ROW_TOP) + (int)g * px(ROW_H);
      if (g % 2) {
         RECT stripe = {0, top, client.right, top + px(ROW_H)};
         HBRUSH b = CreateSolidBrush(RGB(240, 241, 243));
         FillRect(dc, &stripe, b);
         DeleteObject(b);
      }
      int middle = top + px(ROW_H) / 2;
      auto c = countResults(g_guiResults, g_guiGroups[g]->id);
      COLORREF color;
      std::string state;
      if (!complete && g >= g_guiDone) {
         color = g == g_guiDone ? RGB(21, 101, 192) : RGB(176, 176, 176);
         state = g == g_guiDone ? "testing..." : "waiting";
      } else {
         Verdict v = verdict(c);
         color = v == Verdict::Problems ? RGB(198, 40, 40) : v == Verdict::Unsupported ? RGB(117, 117, 117) : RGB(46, 125, 50);
         state = v == Verdict::Problems      ? fmt("FAIL (%d of %d checks)", c[2], c[0] + c[1] + c[2])
                 : v == Verdict::Unsupported ? std::string("NOT SUPPORTED")
                 : v == Verdict::Optional    ? fmt("PASS (%d optional feature%s missing)", c[1], c[1] == 1 ? "" : "s")
                                             : std::string("PASS");
      }
      // The verdict's dot, then the API and the verdict
      HBRUSH dot = CreateSolidBrush(color);
      HPEN pen = CreatePen(PS_SOLID, 1, color);
      SelectObject(dc, dot);
      SelectObject(dc, pen);
      Ellipse(dc, px(MARGIN), middle - px(6), px(MARGIN + 12), middle + px(6));
      DeleteObject(dot);
      DeleteObject(pen);

      SelectObject(dc, text);
      SetTextColor(dc, RGB(32, 32, 32));
      std::wstring name = wide(g_guiGroups[g]->title);
      RECT nameRect = {px(MARGIN + 24), top, px(500), top + px(ROW_H)};
      DrawTextW(dc, name.c_str(), (int)name.size(), &nameRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
      SelectObject(dc, bold);
      SetTextColor(dc, color);
      std::wstring verdictW = wide(state);
      RECT stateRect = {px(512), top, client.right - px(MARGIN), top + px(ROW_H)};
      DrawTextW(dc, verdictW.c_str(), (int)verdictW.size(), &stateRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
   }

   SelectObject(dc, text);
   SetTextColor(dc, RGB(96, 96, 96));
   RECT hint = {px(MARGIN), client.bottom - px(MARGIN + BUTTON_H), guiButton(wnd, 1).left - px(12), client.bottom - px(MARGIN)};
   DrawTextW(dc, L"Click a row for its details.", -1, &hint, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
   const wchar_t *labels[] = {L"Full report", L"Close"};
   for (int b = 0; b < 2; b++) {
      RECT r = guiButton(wnd, b);
      bool enabled = b == 1 || complete;
      HBRUSH fill = CreateSolidBrush(b == 0 && enabled ? RGB(21, 101, 192) : RGB(228, 230, 233));
      HPEN pen = CreatePen(PS_SOLID, 1, b == 0 && enabled ? RGB(21, 101, 192) : RGB(200, 202, 206));
      SelectObject(dc, fill);
      SelectObject(dc, pen);
      RoundRect(dc, r.left, r.top, r.right, r.bottom, px(6), px(6));
      DeleteObject(fill);
      DeleteObject(pen);
      SetTextColor(dc, b == 0 ? (enabled ? RGB(255, 255, 255) : RGB(160, 160, 160)) : RGB(32, 32, 32));
      DrawTextW(dc, labels[b], -1, &r, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
   }
   DeleteObject(text);
   DeleteObject(title);
   DeleteObject(bold);
}

// What a group's row hides: its checks that didn't pass, with their details
static void guiShowGroup(HWND wnd, int row)
{
   std::string body, title;
   {
      std::lock_guard<std::mutex> lock(g_guiLock);
      const Group *group = g_guiGroups[row];
      title = group->title;
      int passed = 0;
      for (auto &r : g_guiResults) {
         if (r.group != group->id) continue;
         if (r.status == "PASS") {
            passed++;
            continue;
         }
         const char *what = r.status == "FAIL" ? "FAILED" : r.status == "WARN" ? "not supported" : r.status == "SKIP" ? "skipped" : "info";
         body += fmt("%s: %s%s%s\n\n", what, r.name.c_str(), r.detail.empty() ? "" : "\n    ", r.detail.c_str());
      }
      if ((size_t)row >= g_guiDone) body = "Not run yet.";
      else body = fmt("%d checks passed.\n\n", passed) + body;
   }
   if (body.size() > 3000) body = body.substr(0, 3000) + "...\n\n(the full report has the rest)";
   MessageBoxW(wnd, wide(body).c_str(), wide(title).c_str(), MB_OK | MB_ICONINFORMATION);
}

static LRESULT CALLBACK guiProc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
   switch (msg) {
   case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(wnd, &ps);
      RECT client;
      GetClientRect(wnd, &client);
      // Drawn off-screen first: no flicker as rows fill in
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
      HGDIOBJ old = SelectObject(mem, bitmap);
      guiPaint(wnd, mem);
      BitBlt(dc, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
      SelectObject(mem, old);
      DeleteObject(bitmap);
      DeleteDC(mem);
      EndPaint(wnd, &ps);
      return 0;
   }
   case WM_ERASEBKGND:
      return 1;
   case WM_LBUTTONUP: {
      POINT p = {(short)LOWORD(lp), (short)HIWORD(lp)};
      RECT report = guiButton(wnd, 0), close = guiButton(wnd, 1);
      std::string html;
      size_t rows;
      {
         std::lock_guard<std::mutex> lock(g_guiLock);
         html = g_guiReport;
         rows = g_guiGroups.size();
      }
      if (PtInRect(&close, p)) {
         DestroyWindow(wnd);
      } else if (PtInRect(&report, p)) {
         if (!html.empty()) ShellExecuteW(wnd, L"open", wide(html).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      } else {
         int row = guiRowAt(p.y, rows);
         if (row >= 0) guiShowGroup(wnd, row);
      }
      return 0;
   }
   case WM_DPICHANGED: {
      g_guiDpi = HIWORD(wp);
      RECT *r = (RECT *)lp;
      SetWindowPos(wnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      return 0;
   }
   case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
   }
   return DefWindowProcW(wnd, msg, wp, lp);
}

static int runGui(DWORD timeoutMs)
{
   // Started from a shortcut, the console is this program's alone: closed (the children run without windows)
   DWORD processes[2];
   if (GetConsoleProcessList(processes, 2) <= 1) FreeConsole();
   // The children in a job that ends with this program: closing the window stops the test running too
   HANDLE job = CreateJobObjectW(nullptr, nullptr);
   JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
   limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
   if (job && SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
      AssignProcessToJobObject(job, GetCurrentProcess()); // (fails in a job already on Windows 7: then they run on)
   }

   HDC screen = GetDC(nullptr);
   g_guiDpi = GetDeviceCaps(screen, LOGPIXELSY);
   ReleaseDC(nullptr, screen);

   size_t rows = 0;
   for (const auto &g : GROUPS) rows += g.check;
   WNDCLASSW wc = {};
   wc.lpfnWndProc = guiProc;
   wc.hInstance = GetModuleHandleW(nullptr);
   wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
   wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
   wc.lpszClassName = L"WinBoatGpuCheck";
   RegisterClassW(&wc);
   RECT r = {0, 0, px(880), px(ROW_TOP) + (int)rows * px(ROW_H) + px(MARGIN * 2 + BUTTON_H + 8)};
   DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
   AdjustWindowRect(&r, style, FALSE);
   g_guiWnd = CreateWindowW(wc.lpszClassName, L"WinBoat GPU Check", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                            r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
   ShowWindow(g_guiWnd, SW_SHOWNORMAL);
   ShowWindow(g_guiWnd, SW_RESTORE); // (a shortcut started minimized: the first call took its state)
   SetForegroundWindow(g_guiWnd);

   g_onProgress = guiProgress;
   std::thread([timeoutMs] {
      runAll(timeoutMs, "");
   }).detach();

   MSG msg;
   while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
   }
   // (closed while testing: the test thread is still running; the job takes the group's child down with this process)
   TerminateProcess(GetCurrentProcess(), 0);
   return 0;
}
