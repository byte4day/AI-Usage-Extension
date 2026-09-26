#include "config.hpp"
#include "util.hpp"

#include <windows.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace cu {

std::wstring Config::ConfigPath() {
  std::wstring dir = AppDataDir() + L"\\CursorUsage";
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir + L"\\config.ini";
}

void Config::Load() {
  auto raw = ReadFileUtf8(ConfigPath());
  if (!raw)
    return;
  std::istringstream in(*raw);
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#' || line[0] == '[')
      continue;
    auto eq = line.find('=');
    if (eq == std::string::npos)
      continue;
    // Trim both ends of both halves. Without stripping the trailing CR, a file
    // that has been through Notepad turns every "=1" into "1\r" and silently
    // disables every boolean setting.
    auto trim = [](std::string& v) {
      const char* ws = " \t\r\n";
      size_t first = v.find_first_not_of(ws);
      if (first == std::string::npos) {
        v.clear();
        return;
      }
      v = v.substr(first, v.find_last_not_of(ws) - first + 1);
    };
    std::string key = line.substr(0, eq);
    std::string val = line.substr(eq + 1);
    trim(key);
    trim(val);

    try {
      if (key == "refresh_interval")
        refreshIntervalSec = (std::clamp)(std::stoi(val), 10, 86400);
      else if (key == "panel_provider")
        panelProvider = val;
      else if (key == "panel_window")
        panelWindow = val;
      else if (key == "usage_display")
        usageDisplay = val;
      else if (key == "show_billing")
        showBilling = (val == "1" || val == "true");
      else if (key == "show_tier")
        showTier = (val != "0" && val != "false");
      else if (key == "show_cursor")
        showCursor = (val != "0" && val != "false");
      else if (key == "show_claude")
        showClaude = (val != "0" && val != "false");
      else if (key == "show_codex")
        showCodex = (val != "0" && val != "false");
      else if (key == "always_on_top")
        alwaysOnTop = (val != "0" && val != "false");
      else if (key == "start_compact")
        startCompact = (val != "0" && val != "false");
      else if (key == "start_hidden")
        startHidden = (val == "1" || val == "true");
      else if (key == "opacity") {
        int o = std::stoi(val);
        opacity = o < 80 ? 80 : (o > 255 ? 255 : o);
      } else if (key == "window_x")
        windowX = std::stoi(val);
      else if (key == "window_y")
        windowY = std::stoi(val);
      else if (key == "proxy_url")
        proxyUrl = Utf8ToWide(val);
    } catch (...) {
    }
  }
}

void Config::Save() const {
  std::ofstream out(ConfigPath().c_str(), std::ios::binary | std::ios::trunc);
  if (!out)
    return;
  out << "refresh_interval=" << refreshIntervalSec << "\n"
      << "panel_provider=" << panelProvider << "\n"
      << "panel_window=" << panelWindow << "\n"
      << "usage_display=" << usageDisplay << "\n"
      << "show_billing=" << (showBilling ? "1" : "0") << "\n"
      << "show_tier=" << (showTier ? "1" : "0") << "\n"
      << "show_cursor=" << (showCursor ? "1" : "0") << "\n"
      << "show_claude=" << (showClaude ? "1" : "0") << "\n"
      << "show_codex=" << (showCodex ? "1" : "0") << "\n"
      << "always_on_top=" << (alwaysOnTop ? "1" : "0") << "\n"
      << "start_compact=" << (startCompact ? "1" : "0") << "\n"
      << "start_hidden=" << (startHidden ? "1" : "0") << "\n"
      << "opacity=" << opacity << "\n"
      << "window_x=" << windowX << "\n"
      << "window_y=" << windowY << "\n"
      << "proxy_url=" << WideToUtf8(proxyUrl) << "\n";
}

}  // namespace cu
