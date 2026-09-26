#include "http.hpp"
#include "util.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winhttp.h>

#include <vector>

namespace cu {
namespace {

// RAII for WinHTTP handles; the original code leaked one on every early return path.
class Handle {
 public:
  Handle() = default;
  explicit Handle(HINTERNET h) : h_(h) {}
  ~Handle() {
    if (h_)
      WinHttpCloseHandle(h_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  Handle& operator=(HINTERNET h) {
    if (h_)
      WinHttpCloseHandle(h_);
    h_ = h;
    return *this;
  }
  operator HINTERNET() const { return h_; }
  explicit operator bool() const { return h_ != nullptr; }

 private:
  HINTERNET h_ = nullptr;
};

}  // namespace

std::string HttpStatusLabel(long status) {
  switch (status) {
    case 401:
    case 403:
      return "Session expired";
    case 404:
      return "Not available";
    case 429:
      return "Rate limited";
    case 0:
      return "No response";
    default:
      break;
  }
  if (status >= 500)
    return "Service down";
  return "HTTP " + std::to_string(status);
}

HttpResponse HttpRequest(const std::wstring& method, const std::wstring& host,
                         const std::wstring& path, const Headers& headers,
                         const std::string& body, const std::wstring& proxyUrl) {
  HttpResponse out;

  Handle session(WinHttpOpen(L"ai-usage-widget/1.3",
                             proxyUrl.empty() ? WINHTTP_ACCESS_TYPE_DEFAULT_PROXY
                                              : WINHTTP_ACCESS_TYPE_NAMED_PROXY,
                             proxyUrl.empty() ? WINHTTP_NO_PROXY_NAME : proxyUrl.c_str(),
                             WINHTTP_NO_PROXY_BYPASS, 0));
  if (!session) {
    out.error = "Network init failed";
    return out;
  }
  WinHttpSetTimeouts(session, 5000, 5000, 15000, 15000);

  Handle connect(WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
  if (!connect) {
    out.error = "Connect failed";
    return out;
  }

  Handle request(WinHttpOpenRequest(connect, method.c_str(), path.c_str(), nullptr,
                                    WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                    WINHTTP_FLAG_SECURE));
  if (!request) {
    out.error = "Request failed";
    return out;
  }

  for (const auto& h : headers) {
    std::wstring line = h.first + L": " + h.second + L"\r\n";
    WinHttpAddRequestHeaders(request, line.c_str(), static_cast<DWORD>(-1L),
                             WINHTTP_ADDREQ_FLAG_ADD);
  }

  const void* payload = body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data();
  DWORD payloadLen = static_cast<DWORD>(body.size());
  if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, const_cast<void*>(payload),
                          payloadLen, payloadLen, 0) ||
      !WinHttpReceiveResponse(request, nullptr)) {
    out.error = "API unreachable";
    return out;
  }

  DWORD status = 0;
  DWORD statusSize = sizeof(status);
  if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                           WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                           WINHTTP_NO_HEADER_INDEX)) {
    out.error = "Invalid HTTP response";
    return out;
  }
  out.status = static_cast<long>(status);

  // Usage endpoints return small JSON objects. Bound a faulty or hostile reply
  // before allocating a buffer from the server-reported available byte count.
  constexpr size_t kMaxResponseBytes = 2 * 1024 * 1024;
  DWORD avail = 0;
  for (;;) {
    if (!WinHttpQueryDataAvailable(request, &avail)) {
      out.error = "Response read failed";
      return out;
    }
    if (avail == 0)
      break;
    if (avail > kMaxResponseBytes - out.body.size()) {
      out.error = "Response too large";
      return out;
    }
    std::vector<char> buf(avail);
    DWORD read = 0;
    if (!WinHttpReadData(request, buf.data(), avail, &read) || read == 0) {
      out.error = "Response read failed";
      return out;
    }
    out.body.append(buf.data(), read);
  }
  return out;
}

HttpResponse HttpGet(const std::wstring& host, const std::wstring& path, const Headers& headers,
                     const std::wstring& proxyUrl) {
  return HttpRequest(L"GET", host, path, headers, std::string(), proxyUrl);
}

}  // namespace cu
