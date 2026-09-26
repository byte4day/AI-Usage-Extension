#include "widget.hpp"
#include "util.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <objidl.h>
#ifndef PROPID
typedef ULONG PROPID;
#endif
#include <gdiplus.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace cu {
namespace {

void CopyTip(wchar_t* dest, size_t destChars, const wchar_t* src) {
  if (!dest || destChars == 0)
    return;
  wcsncpy(dest, src ? src : L"", destChars - 1);
  dest[destChars - 1] = L'\0';
}

int WideToInt(const wchar_t* s) {
  return static_cast<int>(wcstol(s, nullptr, 10));
}

int WindowDpi(HWND hwnd) {
  using Fn = UINT(WINAPI*)(HWND);
  static Fn fn = reinterpret_cast<Fn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
  if (fn) {
    UINT dpi = fn(hwnd);
    if (dpi > 0)
      return static_cast<int>(dpi);
  }
  HDC hdc = GetDC(hwnd);
  int dpi = hdc ? GetDeviceCaps(hdc, LOGPIXELSX) : 96;
  if (hdc)
    ReleaseDC(hwnd, hdc);
  return dpi > 0 ? dpi : 96;
}

bool SystemUsesDarkMode() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0,
                    KEY_READ, &key) != ERROR_SUCCESS)
    return true;
  DWORD value = 1;
  DWORD size = sizeof(value);
  DWORD type = 0;
  bool dark = true;
  if (RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                       reinterpret_cast<LPBYTE>(&value), &size) == ERROR_SUCCESS &&
      type == REG_DWORD)
    dark = value == 0;
  RegCloseKey(key);
  return dark;
}

void UseDarkTitleBar(HWND hwnd, bool dark) {
  BOOL on = dark ? TRUE : FALSE;
  // 20 on current Windows 10/11, 19 on older 1809-1903 builds.
  if (FAILED(DwmSetWindowAttribute(hwnd, 20, &on, sizeof(on))))
    DwmSetWindowAttribute(hwnd, 19, &on, sizeof(on));
}

// --- palette -----------------------------------------------------------------

constexpr COLORREF kCardTop = RGB(34, 35, 40);
constexpr COLORREF kCardBottom = RGB(24, 25, 29);
constexpr COLORREF kBorder = RGB(62, 64, 72);
constexpr COLORREF kDivider = RGB(48, 50, 57);
constexpr COLORREF kTrack = RGB(56, 58, 66);
constexpr COLORREF kText = RGB(238, 239, 243);
constexpr COLORREF kMuted = RGB(150, 153, 163);
constexpr COLORREF kFaint = RGB(112, 115, 125);
constexpr COLORREF kChip = RGB(46, 48, 56);
constexpr COLORREF kChipHot = RGB(64, 67, 77);
constexpr COLORREF kLow = RGB(61, 214, 140);
constexpr COLORREF kHigh = RGB(255, 168, 72);
constexpr COLORREF kCrit = RGB(255, 99, 104);

constexpr COLORREF kCursorAccent = RGB(126, 138, 255);
constexpr COLORREF kClaudeAccent = RGB(217, 119, 87);
constexpr COLORREF kCodexAccent = RGB(87, 191, 158);

constexpr COLORREF kDarkBg = RGB(32, 32, 36);
constexpr COLORREF kDarkText = RGB(232, 233, 238);

COLORREF SeverityColor(double util) {
  if (util >= 90)
    return kCrit;
  if (util >= 75)
    return kHigh;
  return kLow;
}

// Lightened companion used as the far end of each bar's gradient.
COLORREF Lighten(COLORREF c, double amount) {
  auto mix = [&](int v) {
    return static_cast<int>(v + (255 - v) * amount);
  };
  return RGB(mix(GetRValue(c)), mix(GetGValue(c)), mix(GetBValue(c)));
}

Gdiplus::Color ToGp(COLORREF c, BYTE a = 255) {
  return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c));
}

Gdiplus::GraphicsPath* RoundRectPath(const Gdiplus::RectF& r, float radius) {
  auto* path = new Gdiplus::GraphicsPath();
  float d = (std::min)(radius * 2.f, (std::min)(r.Width, r.Height));
  if (d <= 0.f) {
    path->AddRectangle(r);
    return path;
  }
  path->AddArc(r.X, r.Y, d, d, 180, 90);
  path->AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
  path->AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
  path->AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
  path->CloseFigure();
  return path;
}

void FillRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                   const Gdiplus::Brush& brush) {
  Gdiplus::GraphicsPath* path = RoundRectPath(r, radius);
  g.FillPath(&brush, path);
  delete path;
}

void StrokeRoundRect(Gdiplus::Graphics& g, const Gdiplus::RectF& r, float radius,
                     const Gdiplus::Pen& pen) {
  Gdiplus::GraphicsPath* path = RoundRectPath(r, radius);
  g.DrawPath(&pen, path);
  delete path;
}

Gdiplus::RectF ToRectF(const RECT& r) {
  return Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                        static_cast<float>(r.right - r.left),
                        static_cast<float>(r.bottom - r.top));
}

bool PtIn(const RECT& r, int x, int y) {
  return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

float TextWidth(Gdiplus::Graphics& g, const std::wstring& s, const Gdiplus::Font& font) {
  if (s.empty())
    return 0.f;
  Gdiplus::RectF box;
  g.MeasureString(s.c_str(), -1, &font, Gdiplus::PointF(0, 0), &box);
  return box.Width;
}

// Trims with an ellipsis so a long error can never spill out of the card.
std::wstring Ellipsize(Gdiplus::Graphics& g, const std::wstring& s, const Gdiplus::Font& font,
                       float maxWidth) {
  if (s.empty() || TextWidth(g, s, font) <= maxWidth)
    return s;
  std::wstring out = s;
  while (!out.empty() && TextWidth(g, out + L"…", font) > maxWidth)
    out.pop_back();
  return out.empty() ? std::wstring() : out + L"…";
}

void DrawLabel(Gdiplus::Graphics& g, const std::wstring& s, const Gdiplus::Font& font, float x,
              float y, COLORREF color, BYTE alpha = 255) {
  if (s.empty())
    return;
  Gdiplus::SolidBrush brush(ToGp(color, alpha));
  g.DrawString(s.c_str(), -1, &font, Gdiplus::PointF(x, y), &brush);
}

void DrawLabelRight(Gdiplus::Graphics& g, const std::wstring& s, const Gdiplus::Font& font,
                   float right, float y, COLORREF color, BYTE alpha = 255) {
  if (s.empty())
    return;
  DrawLabel(g, s, font, right - TextWidth(g, s, font), y, color, alpha);
}

}  // namespace

// --- preferences -------------------------------------------------------------

namespace {

struct SettingsState {
  Config* config = nullptr;
  bool applied = false;
  bool dark = true;
  int dpi = 96;
  HFONT font = nullptr;
  HFONT boldFont = nullptr;
  HBRUSH backBrush = nullptr;
  HWND refreshEdit = nullptr;
  HWND billingCheck = nullptr;
  HWND tierCheck = nullptr;
  HWND remainingCheck = nullptr;
  HWND compactCheck = nullptr;
  HWND hiddenCheck = nullptr;
  HWND topmostCheck = nullptr;
  HWND cursorCheck = nullptr;
  HWND claudeCheck = nullptr;
  HWND codexCheck = nullptr;
  HWND providerCombo = nullptr;
  HWND poolCombo = nullptr;
  HWND opacitySlider = nullptr;
  HWND opacityValue = nullptr;
  HWND proxyEdit = nullptr;
};

// Builds the dialog contents at the window's real DPI. The previous version
// used fixed pixel offsets and never sent WM_SETFONT, so on a scaled display it
// was both misaligned and rendered in the ancient bitmap system font.
class DialogBuilder {
 public:
  DialogBuilder(HWND parent, SettingsState* state) : parent_(parent), state_(state) {}

  int Scale(int v) const { return MulDiv(v, state_->dpi, 96); }
  int Y() const { return y_; }
  void Advance(int logical) { y_ += Scale(logical); }

  HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int w, int h, int id,
           DWORD exStyle = 0) {
    HWND hwnd = CreateWindowExW(exStyle, cls, text, style | WS_CHILD | WS_VISIBLE, Scale(x),
                                y_, Scale(w), Scale(h), parent_,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
                                nullptr);
    if (hwnd) {
      SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(state_->font), TRUE);
      if (state_->dark)
        SetWindowTheme(hwnd, L"DarkMode_Explorer", nullptr);
    }
    return hwnd;
  }

  void GroupLabel(const wchar_t* text) {
    y_ += Scale(y_ == Scale(kTop) ? 0 : 14);
    HWND hwnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE, Scale(kMargin), y_,
                                Scale(kWidth - kMargin * 2), Scale(18), parent_, nullptr, nullptr,
                                nullptr);
    if (hwnd)
      SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(state_->boldFont), TRUE);
    y_ += Scale(22);
  }

  void Label(const wchar_t* text) {
    Add(L"STATIC", text, 0, kMargin, kWidth - kMargin * 2, 17, 0);
    y_ += Scale(20);
  }

  HWND Check(const wchar_t* text, bool on, int id) {
    HWND hwnd = Add(L"BUTTON", text, WS_TABSTOP | BS_AUTOCHECKBOX, kMargin, kWidth - kMargin * 2,
                    22, id);
    SendMessageW(hwnd, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
    y_ += Scale(25);
    return hwnd;
  }

  HWND Combo(const std::initializer_list<const wchar_t*>& items, int selected, int id) {
    HWND hwnd = Add(L"COMBOBOX", nullptr, WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kMargin,
                    kWidth - kMargin * 2, 160, id);
    for (const wchar_t* item : items)
      SendMessageW(hwnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item));
    SendMessageW(hwnd, CB_SETCURSEL, selected, 0);
    y_ += Scale(30);
    return hwnd;
  }

  static constexpr int kWidth = 340;
  static constexpr int kMargin = 18;
  static constexpr int kTop = 16;

 private:
  HWND parent_;
  SettingsState* state_;
  int y_ = 0;
};

void BuildSettings(HWND hwnd, SettingsState* state) {
  state->dpi = WindowDpi(hwnd);
  state->dark = SystemUsesDarkMode();

  // Use the shell's own UI font at the window's DPI rather than the default
  // bitmap font Win32 hands to freshly created controls.
  NONCLIENTMETRICSW ncm{};
  ncm.cbSize = sizeof(ncm);
  LOGFONTW lf{};
  using SpiFn = BOOL(WINAPI*)(UINT, UINT, PVOID, UINT, UINT);
  auto spi = reinterpret_cast<SpiFn>(reinterpret_cast<void*>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "SystemParametersInfoForDpi")));
  bool got = false;
  if (spi)
    got = spi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, static_cast<UINT>(state->dpi)) != 0;
  if (!got)
    got = SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0) != 0;
  if (got) {
    lf = ncm.lfMessageFont;
    if (!spi)
      lf.lfHeight = MulDiv(lf.lfHeight, state->dpi, 96);
  } else {
    lf.lfHeight = -MulDiv(9, state->dpi, 72);
    wcscpy(lf.lfFaceName, L"Segoe UI");
  }
  state->font = CreateFontIndirectW(&lf);
  lf.lfWeight = FW_SEMIBOLD;
  state->boldFont = CreateFontIndirectW(&lf);
  state->backBrush = CreateSolidBrush(state->dark ? kDarkBg : GetSysColor(COLOR_BTNFACE));

  UseDarkTitleBar(hwnd, state->dark);

  DialogBuilder b(hwnd, state);
  b.Advance(DialogBuilder::kTop);
  const Config& cfg = *state->config;

  b.GroupLabel(L"Providers");
  state->cursorCheck = b.Check(L"Cursor", cfg.showCursor, 110);
  state->claudeCheck = b.Check(L"Claude", cfg.showClaude, 111);
  state->codexCheck = b.Check(L"Codex", cfg.showCodex, 112);

  b.GroupLabel(L"Compact readout");
  b.Label(L"Provider");
  int psel = cfg.panelProvider == "cursor"  ? 1
             : cfg.panelProvider == "claude" ? 2
             : cfg.panelProvider == "codex"  ? 3
                                             : 0;
  state->providerCombo = b.Combo({L"Most used", L"Cursor", L"Claude", L"Codex"}, psel, 109);
  b.Label(L"Pool");
  int sel = cfg.panelWindow == "auto"    ? 1
            : cfg.panelWindow == "api"   ? 2
            : cfg.panelWindow == "total" ? 3
                                         : 0;
  state->poolCombo = b.Combo({L"Most used", L"Primary / Auto / 5h", L"Secondary / API / 7d",
                              L"Total"},
                             sel, 104);
  state->remainingCheck =
      b.Check(L"Count down remaining instead of used", cfg.usageDisplay == "remaining", 103);
  state->tierCheck = b.Check(L"Show plan tier", cfg.showTier, 102);
  state->billingCheck = b.Check(L"Show billing and credits", cfg.showBilling, 101);

  b.GroupLabel(L"Window");
  state->topmostCheck = b.Check(L"Always on top", cfg.alwaysOnTop, 113);
  state->compactCheck = b.Check(L"Start compact", cfg.startCompact, 106);
  state->hiddenCheck = b.Check(L"Start hidden in the tray", cfg.startHidden, 107);

  b.Label(L"Opacity");
  int sliderY = b.Y();
  state->opacitySlider =
      b.Add(TRACKBAR_CLASSW, nullptr, WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, DialogBuilder::kMargin,
            DialogBuilder::kWidth - DialogBuilder::kMargin * 2 - 44, 26, 108);
  SendMessageW(state->opacitySlider, TBM_SETRANGE, TRUE, MAKELPARAM(80, 255));
  SendMessageW(state->opacitySlider, TBM_SETPOS, TRUE, cfg.opacity);
  state->opacityValue = CreateWindowExW(
      0, L"STATIC", std::to_wstring(cfg.opacity * 100 / 255).append(L"%").c_str(),
      WS_CHILD | WS_VISIBLE | SS_RIGHT, b.Scale(DialogBuilder::kWidth - DialogBuilder::kMargin - 40),
      sliderY + b.Scale(4), b.Scale(40), b.Scale(18), hwnd, nullptr, nullptr, nullptr);
  SendMessageW(state->opacityValue, WM_SETFONT, reinterpret_cast<WPARAM>(state->font), TRUE);
  b.Advance(30);

  b.GroupLabel(L"Network");
  b.Label(L"Refresh interval (seconds)");
  state->refreshEdit =
      b.Add(L"EDIT", std::to_wstring(cfg.refreshIntervalSec).c_str(),
            WS_TABSTOP | WS_BORDER | ES_NUMBER, DialogBuilder::kMargin, 100, 24, 100);
  b.Advance(30);
  b.Label(L"Proxy URL (optional)");
  state->proxyEdit = b.Add(L"EDIT", cfg.proxyUrl.c_str(),
                           WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, DialogBuilder::kMargin,
                           DialogBuilder::kWidth - DialogBuilder::kMargin * 2, 24, 105);
  b.Advance(38);

  b.Add(L"BUTTON", L"Cancel", WS_TABSTOP, DialogBuilder::kWidth - DialogBuilder::kMargin - 84, 84,
        28, IDCANCEL);
  b.Add(L"BUTTON", L"Save", WS_TABSTOP | BS_DEFPUSHBUTTON,
        DialogBuilder::kWidth - DialogBuilder::kMargin - 176, 84, 28, IDOK);
  b.Advance(28 + 18);

  // Size the frame to the content instead of hoping a fixed height fits.
  RECT want{0, 0, b.Scale(DialogBuilder::kWidth), b.Y()};
  AdjustWindowRectEx(&want, static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)), FALSE,
                     static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
  SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
               SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void ApplySettings(SettingsState* state) {
  Config& cfg = *state->config;
  wchar_t buf[512];

  GetWindowTextW(state->refreshEdit, buf, 64);
  int interval = WideToInt(buf);
  cfg.refreshIntervalSec = (std::clamp)(interval > 0 ? interval : 300, 10, 86400);

  auto checked = [](HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; };
  cfg.showBilling = checked(state->billingCheck);
  cfg.showTier = checked(state->tierCheck);
  cfg.usageDisplay = checked(state->remainingCheck) ? "remaining" : "used";
  cfg.startCompact = checked(state->compactCheck);
  cfg.startHidden = checked(state->hiddenCheck);
  cfg.alwaysOnTop = checked(state->topmostCheck);
  cfg.showCursor = checked(state->cursorCheck);
  cfg.showClaude = checked(state->claudeCheck);
  cfg.showCodex = checked(state->codexCheck);

  const char* providers[] = {"max", "cursor", "claude", "codex"};
  int psel = static_cast<int>(SendMessageW(state->providerCombo, CB_GETCURSEL, 0, 0));
  if (psel >= 0 && psel < 4)
    cfg.panelProvider = providers[psel];

  const char* pools[] = {"max", "auto", "api", "total"};
  int sel = static_cast<int>(SendMessageW(state->poolCombo, CB_GETCURSEL, 0, 0));
  if (sel >= 0 && sel < 4)
    cfg.panelWindow = pools[sel];

  int opacity = static_cast<int>(SendMessageW(state->opacitySlider, TBM_GETPOS, 0, 0));
  cfg.opacity = (std::max)(80, (std::min)(255, opacity));

  GetWindowTextW(state->proxyEdit, buf, 512);
  cfg.proxyUrl = buf;
  state->applied = true;
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  auto* state = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
    case WM_CREATE: {
      auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      state = static_cast<SettingsState*>(cs->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
      BuildSettings(hwnd, state);
      return 0;
    }
    case WM_CTLCOLORDLG:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
      if (state && state->dark) {
        auto dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kDarkText);
        SetBkColor(dc, kDarkBg);
        return reinterpret_cast<LRESULT>(state->backBrush);
      }
      break;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
      if (state && state->dark) {
        auto dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, kDarkText);
        SetBkColor(dc, RGB(45, 45, 50));
        static HBRUSH editBrush = CreateSolidBrush(RGB(45, 45, 50));
        return reinterpret_cast<LRESULT>(editBrush);
      }
      break;
    case WM_HSCROLL:
      if (state && state->opacitySlider &&
          reinterpret_cast<HWND>(lParam) == state->opacitySlider) {
        int pos = static_cast<int>(SendMessageW(state->opacitySlider, TBM_GETPOS, 0, 0));
        SetWindowTextW(state->opacityValue,
                       std::to_wstring(pos * 100 / 255).append(L"%").c_str());
      }
      return 0;
    case WM_COMMAND:
      if (LOWORD(wParam) == IDOK && state) {
        ApplySettings(state);
        DestroyWindow(hwnd);
        return 0;
      }
      if (LOWORD(wParam) == IDCANCEL) {
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    case WM_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case WM_NCDESTROY:
      if (state) {
        if (state->font)
          DeleteObject(state->font);
        if (state->boldFont)
          DeleteObject(state->boldFont);
        if (state->backBrush)
          DeleteObject(state->backBrush);
        state->font = nullptr;
        state->boldFont = nullptr;
        state->backBrush = nullptr;
      }
      break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

// --- widget ------------------------------------------------------------------

Widget::Widget(HINSTANCE instance) : instance_(instance) {
  config_.Load();
  expanded_ = !config_.startCompact;
  taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");
  wakeMsg_ = RegisterWindowMessageW(L"CursorUsageWidget_Wake_byte4day");
}

Widget::~Widget() {
  DestroyTray();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

int Widget::S(int v) const {
  return MulDiv(v, dpi_, 96);
}

int Widget::Shadow() const {
  return S(kShadow);
}

double Widget::DisplayPct(double util) const {
  return config_.usageDisplay == "remaining" ? 100.0 - util : util;
}

std::wstring Widget::SummaryLabel() const {
  const ProviderUsage* snap = SelectProvider(usage_, config_);
  if (!snap)
    return refreshing_ && !hasUsage_ ? L"…" : L"—";
  if (snap->isUnlimited)
    return L"∞";
  const Pool* p = SelectPool(*snap, config_.panelWindow);
  if (!p)
    return L"—";
  return std::to_wstring(static_cast<int>(std::lround(DisplayPct(p->utilization)))) + L"%";
}

COLORREF Widget::SummaryColor() const {
  const ProviderUsage* snap = SelectProvider(usage_, config_);
  if (!snap)
    return kMuted;
  if (snap->isUnlimited)
    return kLow;
  const Pool* p = SelectPool(*snap, config_.panelWindow);
  return SeverityColor(p ? p->utilization : 0);
}

std::wstring Widget::SummaryProvider() const {
  const ProviderUsage* snap = SelectProvider(usage_, config_);
  if (!snap)
    return error_.empty() ? std::wstring() : Utf8ToWide(error_);
  if (snap->id == "claude")
    return L"Claude";
  if (snap->id == "codex")
    return L"Codex";
  return L"Cursor";
}

std::vector<Section> Widget::BuildSections() const {
  std::vector<Section> sections;

  auto makeRow = [&](const wchar_t* name, const Pool& pool, bool unlimited) {
    MeterRow row;
    row.name = name;
    if (unlimited) {
      row.value = L"unlimited";
      row.muted = true;
      return row;
    }
    if (pool.missing) {
      row.value = L"n/a";
      row.muted = true;
      return row;
    }
    row.util = pool.utilization;
    row.filled = true;
    row.value = std::to_wstring(static_cast<int>(std::lround(DisplayPct(pool.utilization)))) +
                (config_.usageDisplay == "remaining" ? L"% left" : L"% used");
    if (!pool.resetsAt.empty())
      row.caption = Utf8ToWide(RelativeReset(pool.resetsAt));
    return row;
  };

  auto add = [&](bool enabled, const wchar_t* title, COLORREF accent, const ProviderUsage& p,
                 const wchar_t* aName, const wchar_t* bName) {
    if (!enabled)
      return;
    Section s;
    s.title = title;
    s.accent = accent;
    if (!p.ok) {
      // An empty error means the first fetch has not landed yet, which is not
      // the same thing as the provider being unavailable.
      if (p.error.empty()) {
        s.note = refreshing_ ? L"Checking…" : L"No data yet";
        s.noteIsError = false;
      } else {
        s.note = Utf8ToWide(p.error);
        s.noteIsError = true;
      }
      sections.push_back(std::move(s));
      return;
    }
    if (config_.showTier)
      s.tier = Utf8ToWide(p.tier);
    if (p.isUnlimited) {
      s.rows.push_back(makeRow(L"Usage", p.a, true));
    } else {
      if (!p.a.missing || p.b.missing)
        s.rows.push_back(makeRow(aName, p.a, false));
      if (!p.b.missing)
        s.rows.push_back(makeRow(bName, p.b, false));
    }
    if (config_.showBilling && !p.billing.empty())
      s.note = Utf8ToWide(p.billing);
    sections.push_back(std::move(s));
  };

  add(config_.showCursor, L"Cursor", kCursorAccent, usage_.cursor, L"Auto", L"API");
  add(config_.showClaude, L"Claude", kClaudeAccent, usage_.claude, L"5h", L"7d");
  add(config_.showCodex, L"Codex", kCodexAccent, usage_.codex, L"Primary", L"Weekly");
  return sections;
}

int Widget::MeasureSections(const std::vector<Section>& sections) const {
  if (sections.empty())
    return S(44);
  int h = 0;
  for (const Section& s : sections) {
    h += S(kSectionTitleH);
    for (const MeterRow& row : s.rows) {
      h += S(kMeterLabelH) + S(kMeterBarH);
      if (!row.caption.empty())
        h += S(kMeterCaptionH);
      h += S(kMeterGap);
    }
    if (!s.note.empty())
      h += S(kNoteH);
    h += S(kSectionGap);
  }
  return h - S(kSectionGap);
}

Widget::Layout Widget::ComputeLayout() const {
  Layout l;
  l.cardW = S(expanded_ ? kExpandedW : kCompactW);
  l.cardH = expanded_ ? S(kHeaderH) + MeasureSections(BuildSections()) + S(kFooterH)
                      : S(kCompactH);

  const int m = Shadow();
  l.card = RECT{m, m, m + l.cardW, m + l.cardH};

  const int chip = S(20);
  const int chipTop = expanded_ ? l.card.top + S(14) : l.card.top + (S(kCompactH) - chip) / 2;
  int right = l.card.right - S(10);
  l.close = RECT{right - chip, chipTop, right, chipTop + chip};
  right -= chip + S(4);
  l.collapse = RECT{right - chip, chipTop, right, chipTop + chip};

  if (expanded_) {
    const int bh = S(26);
    const int by = l.card.bottom - S(kFooterH) + S(16);
    const int gap = S(6);
    const int widths[3] = {S(64), S(52), S(52)};
    int total = widths[0] + widths[1] + widths[2] + gap * 2;
    int x = l.card.right - S(kPadX) - total;
    for (int i = 0; i < 3; ++i) {
      l.buttons[i] = RECT{x, by, x + widths[i], by + bh};
      x += widths[i] + gap;
    }
  }
  return l;
}

Widget::Hit Widget::HitTest(int x, int y) const {
  Layout l = ComputeLayout();
  if (!PtIn(l.card, x, y))
    return Hit::None;
  // In compact mode the chips only appear on hover, so they only take clicks then.
  if (expanded_ || mouseInside_) {
    if (PtIn(l.close, x, y))
      return Hit::Close;
    if (PtIn(l.collapse, x, y))
      return Hit::ToggleExpand;
  }
  if (expanded_) {
    for (int i = 0; i < 3; ++i) {
      if (PtIn(l.buttons[i], x, y))
        return static_cast<Hit>(static_cast<int>(Hit::Refresh) + i);
    }
  }
  return Hit::Body;
}

void Widget::ClampToWorkArea(int& x, int& y, int w, int h) const {
  POINT probe{x + w / 2, y + h / 2};
  HMONITOR mon = MonitorFromPoint(probe, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi))
    return;
  const RECT& wa = mi.rcWork;
  const int m = Shadow();
  // Keep the visible card on screen; the transparent shadow margin may overhang.
  const int cardW = w - m * 2;
  const int cardH = h - m * 2;
  int cardX = x + m;
  int cardY = y + m;
  cardX = (std::min)((std::max)(cardX, static_cast<int>(wa.left)),
                     static_cast<int>(wa.right) - cardW);
  cardY = (std::min)((std::max)(cardY, static_cast<int>(wa.top)),
                     static_cast<int>(wa.bottom) - cardH);
  x = cardX - m;
  y = cardY - m;
}

void Widget::SnapToEdges(int& x, int& y) const {
  HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(mon, &mi))
    return;
  Layout l = ComputeLayout();
  const int m = Shadow();
  const int snap = S(14);
  const RECT& wa = mi.rcWork;
  const int cardX = x + m;
  const int cardY = y + m;
  if (std::abs(cardX - static_cast<int>(wa.left)) < snap)
    x = wa.left - m + S(10);
  if (std::abs((cardX + l.cardW) - static_cast<int>(wa.right)) < snap)
    x = wa.right - l.cardW - m - S(10);
  if (std::abs(cardY - static_cast<int>(wa.top)) < snap)
    y = wa.top - m + S(10);
  if (std::abs((cardY + l.cardH) - static_cast<int>(wa.bottom)) < snap)
    y = wa.bottom - l.cardH - m - S(10);
}

void Widget::SetExpanded(bool expanded) {
  if (expanded_ == expanded)
    return;
  expanded_ = expanded;
  Relayout();
}

void Widget::Relayout() {
  if (!hwnd_)
    return;
  Layout l = ComputeLayout();
  const int w = l.card.right + Shadow();
  const int h = l.card.bottom + Shadow();
  RECT rc{};
  GetWindowRect(hwnd_, &rc);
  int x = rc.left;
  int y = rc.top;
  ClampToWorkArea(x, y, w, h);
  SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
  Render();
}

void Widget::DrawRing(void* graphics, float cx, float cy, float radius, float stroke, double util,
                      bool known) const {
  auto& g = *static_cast<Gdiplus::Graphics*>(graphics);
  Gdiplus::Pen track(ToGp(kText, 38), stroke);
  track.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
  g.DrawArc(&track, cx - radius, cy - radius, radius * 2, radius * 2, 0, 360);
  if (!known || util <= 0)
    return;
  Gdiplus::Pen arc(ToGp(SeverityColor(util)), stroke);
  arc.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
  float sweep = static_cast<float>((ClampPct(util) / 100.0) * 360.0);
  g.DrawArc(&arc, cx - radius, cy - radius, radius * 2, radius * 2, -90, sweep);
}

void Widget::DrawCard(void* graphics, const Layout& l) {
  auto& g = *static_cast<Gdiplus::Graphics*>(graphics);
  const float scale = dpi_ / 96.f;
  const float radius = static_cast<float>(S(kRadius));
  const Gdiplus::RectF card = ToRectF(l.card);

  // Soft drop shadow: stacked translucent rounded rects, each one pixel larger.
  const int m = Shadow();
  for (int i = m; i >= 1; --i) {
    Gdiplus::SolidBrush brush(Gdiplus::Color(5, 0, 0, 0));
    Gdiplus::RectF r(card.X - i, card.Y - i + S(2), card.Width + i * 2, card.Height + i * 2);
    FillRoundRect(g, r, radius + i, brush);
  }

  Gdiplus::LinearGradientBrush body(
      Gdiplus::RectF(card.X, card.Y - 1, card.Width, card.Height + 2), ToGp(kCardTop),
      ToGp(kCardBottom), Gdiplus::LinearGradientModeVertical);
  FillRoundRect(g, card, radius, body);
  Gdiplus::Pen border(ToGp(kBorder, 170), 1.f);
  StrokeRoundRect(g, Gdiplus::RectF(card.X + 0.5f, card.Y + 0.5f, card.Width - 1, card.Height - 1),
                  radius, border);

  Gdiplus::FontFamily family(L"Segoe UI");
  Gdiplus::Font titleFont(&family, 12.5f * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
  Gdiplus::Font bigFont(&family, 25.f * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
  Gdiplus::Font pillFont(&family, 17.f * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
  Gdiplus::Font sectionFont(&family, 11.5f * scale, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
  Gdiplus::Font bodyFont(&family, 11.5f * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
  Gdiplus::Font smallFont(&family, 10.f * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

  const float left = static_cast<float>(l.card.left + S(kPadX));
  const float right = static_cast<float>(l.card.right - S(kPadX));

  // Chrome chips. In compact mode they only surface while the pointer is over
  // the widget, so the resting state stays a clean pill.
  const bool showChips = expanded_ || mouseInside_;
  if (showChips) {
    auto chip = [&](const RECT& r, const wchar_t* glyph, Hit hit) {
      Gdiplus::SolidBrush fill(ToGp(hover_ == hit ? kChipHot : kChip, hover_ == hit ? 255 : 190));
      FillRoundRect(g, ToRectF(r), static_cast<float>(S(6)), fill);
      Gdiplus::StringFormat center;
      center.SetAlignment(Gdiplus::StringAlignmentCenter);
      center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
      Gdiplus::SolidBrush ink(ToGp(hover_ == hit ? kText : kMuted));
      g.DrawString(glyph, -1, &smallFont, ToRectF(r), &center, &ink);
    };
    chip(l.collapse, expanded_ ? L"▴" : L"▾", Hit::ToggleExpand);
    chip(l.close, L"✕", Hit::Close);
  }

  const ProviderUsage* snap = SelectProvider(usage_, config_);
  double util = 0;
  bool known = false;
  if (snap && !snap->isUnlimited) {
    if (const Pool* p = SelectPool(*snap, config_.panelWindow)) {
      util = p->utilization;
      known = true;
    }
  }

  if (!expanded_) {
    const float cy = card.Y + card.Height / 2.f;
    const float cx = left + S(11);
    DrawRing(&g, cx, cy, static_cast<float>(S(11)), static_cast<float>(S(3)), util, known);

    const std::wstring label = SummaryLabel();
    Gdiplus::RectF lb;
    g.MeasureString(label.c_str(), -1, &pillFont, Gdiplus::PointF(0, 0), &lb);
    const float textX = cx + S(11) + S(10);
    DrawLabel(g, label, pillFont, textX, cy - lb.Height / 2.f, SummaryColor());

    // Right-hand caption trades places with the chrome chips on hover, rather
    // than being squeezed into whatever room the chips leave behind.
    if (!showChips) {
      std::wstring caption = SummaryProvider();
      if (config_.showTier && snap && !snap->tier.empty())
        caption += L" · " + Utf8ToWide(snap->tier);
      const float capMax = right - (textX + lb.Width + S(8));
      if (capMax > S(28)) {
        caption = Ellipsize(g, caption, smallFont, capMax);
        Gdiplus::RectF cb;
        g.MeasureString(caption.c_str(), -1, &smallFont, Gdiplus::PointF(0, 0), &cb);
        DrawLabelRight(g, caption, smallFont, right, cy - cb.Height / 2.f,
                       snap ? kMuted : kHigh);
      }
    }
    return;
  }

  // --- expanded header ---
  DrawLabel(g, L"AI Usage", titleFont, left, card.Y + S(13), kText);
  if (refreshing_)
    DrawLabel(g, L"updating…", smallFont, left + S(72), card.Y + S(16), kFaint);

  const std::wstring summary = SummaryLabel();
  Gdiplus::RectF sb;
  g.MeasureString(summary.c_str(), -1, &bigFont, Gdiplus::PointF(0, 0), &sb);
  DrawLabel(g, summary, bigFont, left, card.Y + S(32), SummaryColor());

  const std::wstring provider = SummaryProvider();
  if (!provider.empty()) {
    DrawLabelRight(g, Ellipsize(g, provider, bodyFont, card.Width * 0.5f), bodyFont, right,
                       card.Y + S(36), kText);
    if (config_.showTier && snap && !snap->tier.empty())
      DrawLabelRight(g, Utf8ToWide(snap->tier), smallFont, right, card.Y + S(53), kFaint);
  }

  Gdiplus::Pen divider(ToGp(kDivider), 1.f);
  const float headerLine = card.Y + S(kHeaderH) - S(10);
  g.DrawLine(&divider, left, headerLine, right, headerLine);

  // --- provider sections ---
  const std::vector<Section> sections = BuildSections();
  float y = card.Y + S(kHeaderH);

  if (sections.empty()) {
    DrawLabel(g, L"No providers enabled", bodyFont, left, y + S(10), kMuted);
    DrawLabel(g, L"Turn one on in Preferences.", smallFont, left, y + S(26), kFaint);
  }

  for (const Section& section : sections) {
    const float dotR = static_cast<float>(S(3));
    Gdiplus::SolidBrush dot(ToGp(section.accent));
    g.FillEllipse(&dot, left, y + S(6), dotR * 2, dotR * 2);
    DrawLabel(g, section.title, sectionFont, left + S(12), y, kText);
    if (!section.tier.empty())
      DrawLabelRight(g, section.tier, smallFont, right, y + S(2), kFaint);
    y += S(kSectionTitleH);

    for (const MeterRow& row : section.rows) {
      const float valueW = TextWidth(g, row.value, bodyFont);
      DrawLabel(g, Ellipsize(g, row.name, bodyFont, right - left - valueW - S(10)), bodyFont, left,
               y, row.muted ? kMuted : kText);
      DrawLabelRight(g, row.value, bodyFont, right, y, row.muted ? kMuted : kText);
      y += S(kMeterLabelH);

      const float barH = static_cast<float>(S(kMeterBarH));
      const Gdiplus::RectF track(left, y, right - left, barH);
      Gdiplus::SolidBrush trackBrush(ToGp(kTrack, 200));
      FillRoundRect(g, track, barH / 2.f, trackBrush);
      if (row.filled) {
        const float fillW = static_cast<float>((ClampPct(row.util) / 100.0) * track.Width);
        if (fillW > 1.f) {
          const COLORREF base = SeverityColor(row.util);
          Gdiplus::LinearGradientBrush fill(Gdiplus::RectF(left - 1, y, fillW + 2, barH),
                                            ToGp(base), ToGp(Lighten(base, 0.22)),
                                            Gdiplus::LinearGradientModeHorizontal);
          FillRoundRect(g, Gdiplus::RectF(left, y, fillW, barH), barH / 2.f, fill);
        }
      }
      y += barH;

      if (!row.caption.empty()) {
        DrawLabel(g, row.caption, smallFont, left, y + S(2), kFaint);
        y += S(kMeterCaptionH);
      }
      y += S(kMeterGap);
    }

    if (!section.note.empty()) {
      DrawLabel(g, Ellipsize(g, section.note, smallFont, right - left), smallFont, left, y,
               section.noteIsError ? kCrit : kFaint);
      y += S(kNoteH);
    }
    y += S(kSectionGap);
  }

  // --- footer ---
  const float footerLine = card.Y + card.Height - S(kFooterH) + S(10);
  g.DrawLine(&divider, left, footerLine, right, footerLine);
  DrawLabel(g, Utf8ToWide(statusLine_), smallFont, left,
            card.Y + card.Height - S(kFooterH) + S(21), kFaint);

  const wchar_t* labels[3] = {L"Refresh", L"Open", L"Prefs"};
  for (int i = 0; i < 3; ++i) {
    const RECT& r = l.buttons[i];
    const bool hot = hover_ == static_cast<Hit>(static_cast<int>(Hit::Refresh) + i);
    Gdiplus::SolidBrush fill(ToGp(hot ? kChipHot : kChip, hot ? 255 : 150));
    FillRoundRect(g, ToRectF(r), static_cast<float>(S(6)), fill);
    Gdiplus::StringFormat center;
    center.SetAlignment(Gdiplus::StringAlignmentCenter);
    center.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::SolidBrush ink(ToGp(hot ? kText : kMuted));
    g.DrawString(labels[i], -1, &smallFont, ToRectF(r), &center, &ink);
  }
}

void Widget::Render() {
  if (!hwnd_)
    return;
  const Layout l = ComputeLayout();
  const int w = l.card.right + Shadow();
  const int h = l.card.bottom + Shadow();
  if (w <= 0 || h <= 0)
    return;

  HDC screen = GetDC(nullptr);
  if (!screen)
    return;
  HDC mem = CreateCompatibleDC(screen);
  if (!mem) {
    ReleaseDC(nullptr, screen);
    return;
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = w;
  bmi.bmiHeader.biHeight = -h;  // top-down
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) {
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return;
  }
  HGDIOBJ old = SelectObject(mem, dib);

  {
    // Wrapping the DIB in a PARGB bitmap makes GDI+ write the premultiplied
    // alpha UpdateLayeredWindow expects, which is what gives genuinely smooth
    // rounded corners instead of the aliased edge SetWindowRgn produced.
    Gdiplus::Bitmap surface(w, h, w * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(bits));
    Gdiplus::Graphics g(&surface);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    DrawCard(&g, l);
  }

  POINT src{0, 0};
  SIZE size{w, h};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, static_cast<BYTE>(config_.opacity), AC_SRC_ALPHA};
  UpdateLayeredWindow(hwnd_, screen, nullptr, &size, mem, &src, 0, &blend, ULW_ALPHA);

  SelectObject(mem, old);
  DeleteObject(dib);
  DeleteDC(mem);
  ReleaseDC(nullptr, screen);
}

bool Widget::Create() {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance_;
  wc.lpszClassName = L"CursorUsageWidget";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
  wc.style = CS_DBLCLKS;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    return false;

  DWORD ex = WS_EX_TOOLWINDOW | WS_EX_LAYERED;
  if (config_.alwaysOnTop)
    ex |= WS_EX_TOPMOST;

  hwnd_ = CreateWindowExW(ex, wc.lpszClassName, L"AI Usage", WS_POPUP, CW_USEDEFAULT,
                          CW_USEDEFAULT, 100, 40, nullptr, nullptr, instance_, this);
  if (!hwnd_)
    return false;

  dpi_ = WindowDpi(hwnd_);

  const Layout l = ComputeLayout();
  const int w = l.card.right + Shadow();
  const int h = l.card.bottom + Shadow();

  int x = config_.windowX;
  int y = config_.windowY;
  if (x == Config::kNoPosition || y == Config::kNoPosition) {
    // Default to the bottom-right of the primary work area.
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    x = wa.right - w - S(8);
    y = wa.bottom - h - S(8);
  }
  // A position saved on a monitor that is no longer attached used to strand the
  // widget off screen with no way back except editing config.ini.
  ClampToWorkArea(x, y, w, h);
  SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);

  Render();
  CreateTray();
  SetTimer(hwnd_, kTimerRefresh, static_cast<UINT>(config_.refreshIntervalSec) * 1000, nullptr);
  ScheduleCountdown();
  RefreshAsync();
  return true;
}

void Widget::Show(bool visible) {
  ShowWindow(hwnd_, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
}

int Widget::Run() {
  Show(!config_.startHidden);
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return static_cast<int>(msg.wParam);
}

LRESULT CALLBACK Widget::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  Widget* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
    self = static_cast<Widget*>(cs->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<Widget*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (!self)
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  return self->HandleMessage(msg, wParam, lParam);
}

void Widget::OpenDashboard() {
  const ProviderUsage* snap = SelectProvider(usage_, config_);
  std::wstring url = L"https://cursor.com/dashboard/usage";
  if (snap && snap->id == "claude")
    url = L"https://claude.ai/settings/usage";
  else if (snap && snap->id == "codex")
    url = L"https://chatgpt.com/codex";
  OpenUrl(url);
}

LRESULT Widget::HandleMessage(UINT msg, WPARAM wParam, LPARAM lParam) {
  if (msg == taskbarCreated_) {
    trayAdded_ = false;
    CreateTray();
    return 0;
  }
  if (wakeMsg_ && msg == wakeMsg_) {
    Show(true);
    SetExpanded(true);
    SetForegroundWindow(hwnd_);
    return 0;
  }

  switch (msg) {
    case WM_NCHITTEST: {
      // Let clicks fall through the transparent shadow margin to whatever is
      // underneath instead of swallowing them in an invisible border.
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      ScreenToClient(hwnd_, &pt);
      const Layout l = ComputeLayout();
      return PtIn(l.card, pt.x, pt.y) ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_DPICHANGED: {
      dpi_ = static_cast<int>(HIWORD(wParam));
      if (dpi_ <= 0)
        dpi_ = 96;
      auto* suggested = reinterpret_cast<RECT*>(lParam);
      const Layout l = ComputeLayout();
      const int w = l.card.right + Shadow();
      const int h = l.card.bottom + Shadow();
      int x = suggested ? suggested->left : 0;
      int y = suggested ? suggested->top : 0;
      ClampToWorkArea(x, y, w, h);
      SetWindowPos(hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
      Render();
      UpdateTray();
      return 0;
    }
    case WM_PAINT: {
      // Content is pushed with UpdateLayeredWindow; just satisfy the loop.
      PAINTSTRUCT ps;
      BeginPaint(hwnd_, &ps);
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;
    case WM_SETCURSOR:
      if (LOWORD(lParam) == HTCLIENT) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        Hit hit = HitTest(pt.x, pt.y);
        if (hit != Hit::None && hit != Hit::Body) {
          SetCursor(LoadCursor(nullptr, IDC_HAND));
          return TRUE;
        }
        SetCursor(LoadCursor(nullptr, dragging_ ? IDC_SIZEALL : IDC_ARROW));
        return TRUE;
      }
      break;
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
      TrackMouseEvent(&tme);
      if (dragging_) {
        POINT cur{};
        GetCursorPos(&cur);
        if (std::abs(cur.x - dragStart_.x) > 3 || std::abs(cur.y - dragStart_.y) > 3)
          movedWhileDrag_ = true;
        int nx = windowStart_.x + (cur.x - dragStart_.x);
        int ny = windowStart_.y + (cur.y - dragStart_.y);
        SnapToEdges(nx, ny);
        SetWindowPos(hwnd_, nullptr, nx, ny, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
      }
      const Hit hit = HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
      const bool inside = hit != Hit::None;
      if (hit != hover_ || inside != mouseInside_) {
        hover_ = hit;
        mouseInside_ = inside;
        Render();
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hover_ != Hit::None || mouseInside_) {
        hover_ = Hit::None;
        mouseInside_ = false;
        Render();
      }
      return 0;
    case WM_LBUTTONDOWN: {
      switch (HitTest(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam))) {
        case Hit::Refresh:
          RefreshAsync();
          return 0;
        case Hit::Open:
          OpenDashboard();
          return 0;
        case Hit::Prefs:
          OpenSettings();
          return 0;
        case Hit::Close:
          Show(false);
          return 0;
        case Hit::ToggleExpand:
          SetExpanded(!expanded_);
          return 0;
        case Hit::None:
          return 0;
        default:
          break;
      }
      dragging_ = true;
      movedWhileDrag_ = false;
      SetCapture(hwnd_);
      GetCursorPos(&dragStart_);
      RECT rc;
      GetWindowRect(hwnd_, &rc);
      windowStart_ = {rc.left, rc.top};
      return 0;
    }
    case WM_LBUTTONUP:
      if (dragging_) {
        dragging_ = false;
        ReleaseCapture();
        PersistPosition();
        if (!movedWhileDrag_ && !expanded_)
          SetExpanded(true);
      }
      return 0;
    case WM_LBUTTONDBLCLK:
      return 0;
    case WM_RBUTTONUP:
      ShowContextMenu();
      return 0;
    case WM_TIMER:
      if (wParam == kTimerRefresh) {
        RefreshAsync();
      } else if (wParam == kTimerCountdown) {
        Render();
        ScheduleCountdown();
      }
      return 0;
    case kMsgRefreshDone: {
      auto* result = reinterpret_cast<AllUsage*>(lParam);
      refreshing_ = false;
      ApplyUsage(*result);
      delete result;
      Relayout();
      UpdateTray();
      ScheduleCountdown();
      return 0;
    }
    case kTrayMsg:
      if (lParam == WM_LBUTTONUP || lParam == NIN_SELECT) {
        const bool visible = IsWindowVisible(hwnd_);
        Show(!visible);
        if (!visible) {
          SetExpanded(true);
          SetForegroundWindow(hwnd_);
        }
      } else if (lParam == WM_RBUTTONUP || lParam == WM_CONTEXTMENU) {
        ShowContextMenu();
      }
      return 0;
    case WM_COMMAND:
      switch (LOWORD(wParam)) {
        case 1001:
          RefreshAsync();
          break;
        case 1002:
          OpenDashboard();
          break;
        case 1003:
          OpenSettings();
          break;
        case 1004:
          Show(!IsWindowVisible(hwnd_));
          break;
        case 1005:
          DestroyWindow(hwnd_);
          break;
        case 1006:
          config_.alwaysOnTop = !config_.alwaysOnTop;
          SetWindowPos(hwnd_, config_.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                       SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
          config_.Save();
          break;
        case 1007:
          SetExpanded(!expanded_);
          break;
      }
      return 0;
    case WM_DESTROY:
      PersistPosition();
      KillTimer(hwnd_, kTimerRefresh);
      KillTimer(hwnd_, kTimerCountdown);
      DestroyTray();
      hwnd_ = nullptr;
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd_, msg, wParam, lParam);
}

void Widget::RefreshAsync() {
  if (refreshing_ || !hwnd_)
    return;
  refreshing_ = true;
  Render();

  HWND hwnd = hwnd_;
  Config cfg = config_;
  std::thread([hwnd, cfg]() {
    auto* result = new AllUsage(FetchAllUsage(cfg));
    // PostMessage can fail if the window went away while we were fetching;
    // without this the result was simply leaked.
    if (!IsWindow(hwnd) || !PostMessageW(hwnd, kMsgRefreshDone, 0,
                                         reinterpret_cast<LPARAM>(result)))
      delete result;
  }).detach();
}

void Widget::ApplyUsage(const AllUsage& result) {
  usage_ = result;
  hasUsage_ = usage_.cursor.ok || usage_.claude.ok || usage_.codex.ok;
  error_.clear();
  if (!hasUsage_) {
    for (const ProviderUsage* p : {&usage_.cursor, &usage_.claude, &usage_.codex}) {
      if (!p->error.empty()) {
        error_ = p->error;
        break;
      }
    }
    if (error_.empty())
      error_ = "No providers";
    statusLine_ = "Checked " + FormatTimeNow();
  } else {
    statusLine_ = "Updated " + FormatTimeNow();
  }
}

void Widget::ScheduleCountdown() {
  KillTimer(hwnd_, kTimerCountdown);
  UINT ms = 30000;
  if (const ProviderUsage* snap = SelectProvider(usage_, config_)) {
    const Pool* p = SelectPool(*snap, config_.panelWindow);
    if (p && !p->resetsAt.empty()) {
      std::optional<int64_t> secs;
      if (p->resetsAt.rfind("unix:", 0) == 0) {
        try {
          const long long target = std::stoll(p->resetsAt.substr(5));
          const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
          secs = static_cast<int64_t>(target - now);
        } catch (...) {
        }
      } else {
        secs = SecondsUntilIso(p->resetsAt);
      }
      if (secs && *secs > 0 && *secs < 90)
        ms = 1000;
    }
  }
  SetTimer(hwnd_, kTimerCountdown, ms, nullptr);
}

HICON Widget::MakeTrayIcon(int size) {
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = size;
  bmi.bmiHeader.biHeight = -size;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP dib = CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!dib || !bits)
    return LoadIcon(nullptr, IDI_APPLICATION);

  {
    Gdiplus::Bitmap surface(size, size, size * 4, PixelFormat32bppPARGB,
                            static_cast<BYTE*>(bits));
    Gdiplus::Graphics g(&surface);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    double util = 0;
    bool known = false;
    if (const ProviderUsage* snap = SelectProvider(usage_, config_)) {
      if (!snap->isUnlimited) {
        if (const Pool* p = SelectPool(*snap, config_.panelWindow)) {
          util = p->utilization;
          known = true;
        }
      }
    }
    const float stroke = (std::max)(2.f, size / 8.f);
    const float radius = size / 2.f - stroke / 2.f - 1.f;
    DrawRing(&g, size / 2.f, size / 2.f, radius, stroke, util, known);
    if (!hasUsage_) {
      Gdiplus::Pen warn(ToGp(kHigh), stroke);
      g.DrawArc(&warn, size / 2.f - radius, size / 2.f - radius, radius * 2, radius * 2, -90, 90);
    }
  }

  ICONINFO ii{};
  ii.fIcon = TRUE;
  ii.hbmColor = dib;
  // Fully-opaque mask: the alpha in hbmColor is what shapes a 32bpp icon.
  ii.hbmMask = CreateBitmap(size, size, 1, 1, nullptr);
  HICON icon = CreateIconIndirect(&ii);
  DeleteObject(ii.hbmMask);
  DeleteObject(dib);
  return icon ? icon : LoadIcon(nullptr, IDI_APPLICATION);
}

void Widget::CreateTray() {
  if (trayAdded_)
    return;
  if (trayIcon_) {
    DestroyIcon(trayIcon_);
    trayIcon_ = nullptr;
  }
  // Match the shell's small-icon metric instead of assuming 16px, which was
  // blurry on any scaled display.
  trayIcon_ = MakeTrayIcon((std::max)(16, GetSystemMetrics(SM_CXSMICON)));
  nid_ = {};
  nid_.cbSize = sizeof(nid_);
  nid_.hWnd = hwnd_;
  nid_.uID = 1;
  nid_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  nid_.uCallbackMessage = kTrayMsg;
  nid_.hIcon = trayIcon_;
  CopyTip(nid_.szTip, sizeof(nid_.szTip) / sizeof(nid_.szTip[0]), L"AI Usage");
  trayAdded_ = Shell_NotifyIconW(NIM_ADD, &nid_) == TRUE;
}

void Widget::DestroyTray() {
  if (trayAdded_) {
    Shell_NotifyIconW(NIM_DELETE, &nid_);
    trayAdded_ = false;
  }
  if (trayIcon_) {
    DestroyIcon(trayIcon_);
    trayIcon_ = nullptr;
  }
}

void Widget::UpdateTray() {
  if (!trayAdded_) {
    CreateTray();
    return;
  }
  std::wstring tip = L"AI Usage";
  if (const ProviderUsage* snap = SelectProvider(usage_, config_)) {
    if (snap->isUnlimited) {
      tip += L" — unlimited";
    } else if (const Pool* p = SelectPool(*snap, config_.panelWindow)) {
      tip += L" — " + std::to_wstring(static_cast<int>(std::lround(DisplayPct(p->utilization)))) +
             L"%";
      if (config_.showTier && !snap->tier.empty())
        tip += L" · " + Utf8ToWide(snap->tier);
    }
  } else if (!error_.empty()) {
    tip += L" — " + Utf8ToWide(error_);
  }
  if (tip.size() >= 127)
    tip.resize(127);
  CopyTip(nid_.szTip, sizeof(nid_.szTip) / sizeof(nid_.szTip[0]), tip.c_str());

  HICON fresh = MakeTrayIcon((std::max)(16, GetSystemMetrics(SM_CXSMICON)));
  if (trayIcon_)
    DestroyIcon(trayIcon_);
  trayIcon_ = fresh;
  nid_.hIcon = trayIcon_;
  Shell_NotifyIconW(NIM_MODIFY, &nid_);
}

void Widget::ShowContextMenu() {
  POINT pt;
  GetCursorPos(&pt);
  HMENU menu = CreatePopupMenu();
  AppendMenuW(menu, MF_STRING, 1004, IsWindowVisible(hwnd_) ? L"Hide widget" : L"Show widget");
  AppendMenuW(menu, MF_STRING, 1007, expanded_ ? L"Compact view" : L"Expanded view");
  AppendMenuW(menu, MF_STRING | (config_.alwaysOnTop ? MF_CHECKED : 0), 1006, L"Always on top");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 1001, L"Refresh");
  AppendMenuW(menu, MF_STRING, 1002, L"Open dashboard");
  AppendMenuW(menu, MF_STRING, 1003, L"Preferences…");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, 1005, L"Exit");
  SetForegroundWindow(hwnd_);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

void Widget::PersistPosition() {
  if (!hwnd_)
    return;
  RECT rc;
  GetWindowRect(hwnd_, &rc);
  config_.windowX = rc.left;
  config_.windowY = rc.top;
  config_.Save();
}

void Widget::OpenSettings() {
  // The tray menu stays live while the widget window is disabled, so without
  // this guard a second Preferences window could nest another modal loop.
  if (settingsOpen_)
    return;

  static bool registered = false;
  if (!registered) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = SettingsWndProc;
    wc.hInstance = instance_;
    wc.lpszClassName = L"CursorUsageSettings";
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
      return;
    registered = true;
  }

  Config copy = config_;
  SettingsState state;
  state.config = &copy;

  HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"CursorUsageSettings", L"AI Usage preferences",
                             WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT,
                             CW_USEDEFAULT, 400, 600, hwnd_, nullptr, instance_, &state);
  if (!dlg)
    return;

  // Centre on the widget's monitor, then show.
  RECT dr{};
  GetWindowRect(dlg, &dr);
  HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (GetMonitorInfoW(mon, &mi)) {
    const int w = dr.right - dr.left;
    const int h = dr.bottom - dr.top;
    SetWindowPos(dlg, nullptr, mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - w) / 2,
                 mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - h) / 2, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  ShowWindow(dlg, SW_SHOW);
  SetForegroundWindow(dlg);

  settingsOpen_ = true;
  EnableWindow(hwnd_, FALSE);
  MSG msg;
  bool quitting = false;
  while (IsWindow(dlg)) {
    BOOL got = GetMessageW(&msg, nullptr, 0, 0);
    if (got <= 0) {
      quitting = true;
      break;
    }
    if (!IsDialogMessageW(dlg, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  EnableWindow(hwnd_, TRUE);
  settingsOpen_ = false;
  if (quitting) {
    // WM_QUIT arrived while this nested loop owned the queue; hand it back so
    // the main loop still terminates.
    PostQuitMessage(static_cast<int>(msg.wParam));
    return;
  }
  SetForegroundWindow(hwnd_);

  if (!state.applied)
    return;

  const bool topmostChanged = copy.alwaysOnTop != config_.alwaysOnTop;
  config_ = copy;
  config_.Save();
  if (topmostChanged) {
    SetWindowPos(hwnd_, config_.alwaysOnTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  KillTimer(hwnd_, kTimerRefresh);
  SetTimer(hwnd_, kTimerRefresh, static_cast<UINT>(config_.refreshIntervalSec) * 1000, nullptr);
  Relayout();
  UpdateTray();
  RefreshAsync();
}

}  // namespace cu
