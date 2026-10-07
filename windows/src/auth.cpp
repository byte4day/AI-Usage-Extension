#include "auth.hpp"
#include "http.hpp"
#include "util.hpp"

#include <windows.h>
#include <wincred.h>

#include <algorithm>
#include <string>
#include <vector>

namespace cu {

static std::optional<std::string> TokenFromAuthJson(const std::wstring& path) {
  auto raw = ReadFileUtf8(path);
  if (!raw)
    return std::nullopt;
  auto token = JsonString(*raw, "accessToken");
  if (token.empty())
    token = JsonString(*raw, "access_token");
  if (token.empty())
    return std::nullopt;
  return token;
}

static std::optional<std::string> TokenFromEnv(const wchar_t* name) {
  wchar_t buf[4096];
  DWORD n = GetEnvironmentVariableW(name, buf, 4096);
  if (n == 0 || n >= 4096)
    return std::nullopt;
  std::wstring w(buf);
  while (!w.empty() && (w.back() == L' ' || w.back() == L'\t'))
    w.pop_back();
  if (w.empty())
    return std::nullopt;
  return WideToUtf8(w);
}

static std::optional<std::string> TokenFromSqlite(const std::wstring& dbPath) {
  if (!FileExists(dbPath))
    return std::nullopt;

  std::wstring cmd = L"sqlite3 \"" + dbPath +
                     L"\" \"SELECT value FROM ItemTable WHERE key='cursorAuth/accessToken' LIMIT 1;\"";

  SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
  HANDLE readPipe = nullptr, writePipe = nullptr;
  if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    return std::nullopt;
  SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
  si.hStdOutput = writePipe;
  si.hStdError = writePipe;
  si.wShowWindow = SW_HIDE;

  PROCESS_INFORMATION pi{};
  std::wstring mutableCmd = cmd;
  BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                           nullptr, nullptr, &si, &pi);
  CloseHandle(writePipe);
  if (!ok) {
    CloseHandle(readPipe);
    return std::nullopt;
  }

  // Never block forever on ReadFile — a locked state.vscdb or hung sqlite3 used
  // to freeze the refresh worker and leave the tray stuck on "updating…".
  std::string out;
  char buf[4096];
  const DWORD deadline = GetTickCount() + 5000;
  bool alive = true;
  while (alive && GetTickCount() < deadline) {
    DWORD avail = 0;
    if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr))
      break;
    if (avail > 0) {
      DWORD n = 0;
      const DWORD want = (std::min)(avail, static_cast<DWORD>(sizeof(buf)));
      if (!ReadFile(readPipe, buf, want, &n, nullptr) || n == 0)
        break;
      out.append(buf, buf + n);
      continue;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, 50);
    if (wait == WAIT_OBJECT_0)
      alive = false;
  }
  if (WaitForSingleObject(pi.hProcess, 0) != WAIT_OBJECT_0) {
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 1000);
  }
  DWORD avail = 0;
  while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr) && avail > 0) {
    DWORD n = 0;
    const DWORD want = (std::min)(avail, static_cast<DWORD>(sizeof(buf)));
    if (!ReadFile(readPipe, buf, want, &n, nullptr) || n == 0)
      break;
    out.append(buf, buf + n);
  }

  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  CloseHandle(readPipe);

  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
    out.pop_back();
  if (out.empty())
    return std::nullopt;
  return out;
}

std::optional<std::string> ResolveCursorToken() {
  if (auto env = TokenFromEnv(L"CURSOR_SESSION_TOKEN")) {
    std::string e = *env;
    auto cut = e.find("::");
    if (cut == std::string::npos)
      cut = e.find("%3A%3A");
    if (cut != std::string::npos) {
      if (e.compare(cut, 2, "::") == 0)
        return e.substr(cut + 2);
      return e.substr(cut + 6);
    }
    return e;
  }

  const std::wstring candidates[] = {
      HomeDir() + L"\\.cursor\\auth.json",
      AppDataDir() + L"\\cursor\\auth.json",
      LocalAppDataDir() + L"\\cursor\\auth.json",
  };
  for (const auto& path : candidates) {
    if (auto t = TokenFromAuthJson(path))
      return t;
  }

  return TokenFromSqlite(AppDataDir() + L"\\Cursor\\User\\globalStorage\\state.vscdb");
}

std::optional<std::string> SessionCookieFromToken(const std::string& accessToken) {
  auto payload = JwtPayloadJson(accessToken);
  if (!payload)
    return std::nullopt;
  auto sub = JsonString(*payload, "sub");
  if (sub.empty())
    return std::nullopt;
  auto pipe = sub.rfind('|');
  if (pipe != std::string::npos)
    sub = sub.substr(pipe + 1);
  return sub + "%3A%3A" + accessToken;
}

namespace {

// Claude Code refreshes against api.anthropic.com — console.anthropic.com
// rate-limits this client id and leaves the widget stuck on expired auth.
constexpr wchar_t kClaudeTokenHost[] = L"api.anthropic.com";
constexpr wchar_t kClaudeTokenPath[] = L"/v1/oauth/token";
constexpr char kClaudeClientId[] = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";

// Renew a little early so a refresh mid-request cannot 401 us.
constexpr int64_t kExpirySkewMs = 120 * 1000;

// Where the credential blob came from, so a rotated token can be written back
// to exactly the store the CLI will read next.
struct CredStore {
  enum class Kind { File, Vault } kind = Kind::File;
  std::wstring locator;  // file path, or Credential Manager target name
  std::string raw;       // verbatim JSON, so unknown fields survive a rewrite
};

std::string BlobToUtf8(const BYTE* blob, DWORD bytes) {
  if (!blob || bytes == 0)
    return {};
  // Credential Manager blobs are raw bytes; Claude Code writes UTF-8, but other
  // writers use UTF-16. A NUL in an even position is the giveaway.
  bool wide = bytes >= 2 && (bytes % 2 == 0) && blob[1] == 0;
  if (wide) {
    std::wstring w(reinterpret_cast<const wchar_t*>(blob), bytes / sizeof(wchar_t));
    while (!w.empty() && w.back() == L'\0')
      w.pop_back();
    return WideToUtf8(w);
  }
  std::string s(reinterpret_cast<const char*>(blob), bytes);
  while (!s.empty() && s.back() == '\0')
    s.pop_back();
  return s;
}

bool LooksLikeClaudeCreds(const std::string& raw) {
  if (raw.find("claudeAiOauth") != std::string::npos)
    return true;
  return raw.find("accessToken") != std::string::npos &&
         raw.find("refreshToken") != std::string::npos;
}

std::optional<CredStore> ReadVault(const std::wstring& target) {
  PCREDENTIALW cred = nullptr;
  if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred) || !cred)
    return std::nullopt;
  CredStore store;
  store.kind = CredStore::Kind::Vault;
  store.locator = target;
  store.raw = BlobToUtf8(cred->CredentialBlob, cred->CredentialBlobSize);
  CredFree(cred);
  if (store.raw.empty())
    return std::nullopt;
  return store;
}

bool WriteVault(const std::wstring& target, const std::string& raw) {
  // CredWriteW replaces the whole record, so build the new one from the record
  // that is already there and swap only the blob. Writing a zero-initialised
  // CREDENTIALW would strip the CLI's own UserName, Comment, Attributes and
  // persistence scope. The shallow copy is safe because every borrowed pointer
  // stays alive until CredFree below.
  PCREDENTIALW existing = nullptr;
  if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &existing) && existing) {
    CREDENTIALW updated = *existing;
    updated.CredentialBlobSize = static_cast<DWORD>(raw.size());
    updated.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(raw.data()));
    const BOOL ok = CredWriteW(&updated, 0);
    CredFree(existing);
    return ok == TRUE;
  }

  CREDENTIALW cred{};
  cred.Type = CRED_TYPE_GENERIC;
  cred.TargetName = const_cast<wchar_t*>(target.c_str());
  cred.CredentialBlobSize = static_cast<DWORD>(raw.size());
  cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(raw.data()));
  cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
  return CredWriteW(&cred, 0) == TRUE;
}

// Every place a Claude Code install may keep its credentials, most-likely first.
std::vector<CredStore> DiscoverClaudeStores() {
  std::vector<CredStore> found;

  const std::wstring files[] = {
      HomeDir() + L"\\.claude\\.credentials.json",
      AppDataDir() + L"\\Claude\\.credentials.json",
      AppDataDir() + L"\\claude\\.credentials.json",
      LocalAppDataDir() + L"\\Claude\\.credentials.json",
      HomeDir() + L"\\.config\\claude\\.credentials.json",
  };
  for (const auto& path : files) {
    auto raw = ReadFileUtf8(path);
    if (raw && LooksLikeClaudeCreds(*raw)) {
      CredStore store;
      store.kind = CredStore::Kind::File;
      store.locator = path;
      store.raw = *raw;
      found.push_back(std::move(store));
    }
  }

  const std::wstring targets[] = {
      L"Claude Code-credentials",
      L"Claude Code",
      L"claude-code",
  };
  for (const auto& target : targets) {
    if (auto store = ReadVault(target)) {
      if (LooksLikeClaudeCreds(store->raw))
        found.push_back(std::move(*store));
    }
  }

  // The vault entry has been renamed across CLI versions, so rather than keep
  // guessing, ask the vault what Claude-prefixed entries actually exist.
  DWORD count = 0;
  PCREDENTIALW* list = nullptr;
  if (CredEnumerateW(L"Claude*", 0, &count, &list) && list) {
    for (DWORD i = 0; i < count; ++i) {
      if (!list[i] || list[i]->Type != CRED_TYPE_GENERIC)
        continue;
      std::wstring target = list[i]->TargetName ? list[i]->TargetName : L"";
      bool seen = false;
      for (const auto& s : found)
        seen = seen || (s.kind == CredStore::Kind::Vault && s.locator == target);
      if (seen)
        continue;
      std::string raw = BlobToUtf8(list[i]->CredentialBlob, list[i]->CredentialBlobSize);
      if (!LooksLikeClaudeCreds(raw))
        continue;
      CredStore store;
      store.kind = CredStore::Kind::Vault;
      store.locator = target;
      store.raw = std::move(raw);
      found.push_back(std::move(store));
    }
    CredFree(list);
  }

  return found;
}

// The OAuth fields live under "claudeAiOauth" in current CLI versions; older
// ones stored them at the top level.
std::string OauthSection(const std::string& raw) {
  auto section = JsonObject(raw, "claudeAiOauth");
  return section.empty() ? raw : section;
}

bool PersistStore(const CredStore& store, const std::string& raw) {
  if (store.kind == CredStore::Kind::Vault)
    return WriteVault(store.locator, raw);
  return WriteFileAtomic(store.locator, raw);
}

// Swaps the rotated values into the original document by byte range, so fields
// we do not model (scopes, rateLimitTier, other accounts) are preserved exactly.
std::optional<std::string> SpliceRotated(const std::string& raw, const std::string& accessToken,
                                         const std::string& refreshToken, int64_t expiresAtMs) {
  std::string doc = raw;
  JsonSpan section = JsonValueSpan(doc, "claudeAiOauth");
  size_t begin = section.found ? section.begin : 0;
  size_t end = section.found ? section.end : doc.size();

  std::string body = doc.substr(begin, end - begin);
  if (!JsonSetString(body, "accessToken", accessToken))
    return std::nullopt;
  if (!refreshToken.empty())
    JsonSetString(body, "refreshToken", refreshToken);
  if (!JsonSetNumber(body, "expiresAt", expiresAtMs)) {
    // Older dumps omit expiresAt; insert it so the next launch renews on time.
    const auto close = body.rfind('}');
    if (close == std::string::npos)
      return std::nullopt;
    std::string insert = "\"expiresAt\":" + std::to_string(expiresAtMs);
    size_t i = close;
    while (i > 0 && (body[i - 1] == ' ' || body[i - 1] == '\n' || body[i - 1] == '\r' ||
                     body[i - 1] == '\t'))
      --i;
    if (i > 0 && body[i - 1] != '{' && body[i - 1] != ',')
      insert = "," + insert;
    body.insert(close, insert);
  }

  doc.replace(begin, end - begin, body);
  return doc;
}

std::optional<ClaudeCred> RefreshStore(CredStore& store, const std::wstring& proxyUrl,
                                       std::string* reason) {
  const std::string section = OauthSection(store.raw);
  const std::string refreshToken = JsonString(section, "refreshToken");
  if (refreshToken.empty()) {
    if (reason)
      *reason = "Session expired";
    return std::nullopt;
  }

  const std::string payload = std::string("{\"grant_type\":\"refresh_token\",\"refresh_token\":\"") +
                              JsonEscape(refreshToken) + "\",\"client_id\":\"" + kClaudeClientId +
                              "\"}";

  auto response = HttpRequest(L"POST", kClaudeTokenHost, kClaudeTokenPath,
                              {{L"Content-Type", L"application/json"},
                               {L"Accept", L"application/json"},
                               {L"User-Agent", L"claude-code/2.1.72"}},
                              payload, proxyUrl);
  if (!response.ok()) {
    if (reason)
      *reason = response.error.empty() ? HttpStatusLabel(response.status) : response.error;
    return std::nullopt;
  }

  const std::string fresh = JsonString(response.body, "access_token");
  if (fresh.empty()) {
    if (reason)
      *reason = "Refresh rejected";
    return std::nullopt;
  }
  const std::string rotated = JsonString(response.body, "refresh_token");
  const double expiresIn = JsonNumber(response.body, "expires_in").value_or(8 * 3600);
  const int64_t expiresAt = UnixMillisNow() + static_cast<int64_t>(expiresIn) * 1000;

  auto updated = SpliceRotated(store.raw, fresh, rotated, expiresAt);
  // The server has already invalidated the old refresh token. If we cannot save
  // the rotated one, using the new access token would silently log the CLI out
  // on its next start, so bail instead and leave the stored credentials alone.
  if (!updated || !PersistStore(store, *updated)) {
    if (reason)
      *reason = "Cannot save token";
    return std::nullopt;
  }
  store.raw = *updated;

  ClaudeCred cred;
  cred.accessToken = fresh;
  cred.subscriptionType = JsonString(OauthSection(store.raw), "subscriptionType");
  cred.refreshed = true;
  return cred;
}

}  // namespace

std::optional<ClaudeCred> ResolveClaudeCred(const std::wstring& proxyUrl, std::string* reason,
                                            bool force) {
  if (reason)
    reason->clear();

  if (auto env = TokenFromEnv(L"CLAUDE_CODE_OAUTH_TOKEN"))
    return ClaudeCred{*env, "CLAUDE", false};

  auto stores = DiscoverClaudeStores();
  if (stores.empty()) {
    if (reason)
      *reason = "Run claude to sign in";
    return std::nullopt;
  }

  // Serialise refreshes so two widget instances cannot both spend the same
  // single-use refresh token and knock each other out. If we cannot take the
  // lock we must not refresh at all, or we would race the holder for it.
  HANDLE lock = CreateMutexW(nullptr, FALSE, L"Local\\AIUsageWidget.ClaudeRefresh");
  bool owned = false;
  if (lock) {
    const DWORD wait = WaitForSingleObject(lock, 10000);
    owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    if (!owned) {
      CloseHandle(lock);
      lock = nullptr;
    }
  }
  struct LockGuard {
    HANDLE h;
    ~LockGuard() {
      if (h) {
        ReleaseMutex(h);
        CloseHandle(h);
      }
    }
  } guard{lock};

  const int64_t now = UnixMillisNow();
  std::optional<ClaudeCred> expiredCandidate;
  CredStore* refreshable = nullptr;

  for (auto& store : stores) {
    // Re-read under the lock: another instance may have rotated it while we
    // were waiting, in which case there is nothing left to do.
    if (store.kind == CredStore::Kind::File) {
      if (auto raw = ReadFileUtf8(store.locator))
        store.raw = *raw;
    } else if (auto fresh = ReadVault(store.locator)) {
      store.raw = fresh->raw;
    }

    const std::string section = OauthSection(store.raw);
    const std::string token = JsonString(section, "accessToken");
    if (token.empty())
      continue;

    const auto expiresAtRaw = JsonNumber(section, "expiresAt");
    // Claude stores expiresAt in ms; older dumps sometimes used seconds.
    int64_t expiresAtMs = 0;
    if (expiresAtRaw && *expiresAtRaw > 0) {
      expiresAtMs = static_cast<int64_t>(*expiresAtRaw);
      if (expiresAtMs < 1'000'000'000'000LL)
        expiresAtMs *= 1000;
    }
    const bool expired =
        force || (expiresAtMs > 0 && expiresAtMs - kExpirySkewMs <= now);
    if (!expired)
      return ClaudeCred{token, JsonString(section, "subscriptionType"), false};

    if (!refreshable && !JsonString(section, "refreshToken").empty())
      refreshable = &store;
    if (!expiredCandidate)
      expiredCandidate = ClaudeCred{token, JsonString(section, "subscriptionType"), false};
  }

  if (refreshable && owned) {
    if (auto renewed = RefreshStore(*refreshable, proxyUrl, reason))
      return renewed;
  } else if (refreshable && reason) {
    *reason = "Refreshing";
  }

  // Refresh was impossible or failed. The stale token is still worth a try —
  // clock skew, or a server that has not actually expired it yet. When the
  // caller forced a refresh the server has already refused it, so do not retry.
  if (expiredCandidate && !force) {
    if (reason)
      reason->clear();
    return expiredCandidate;
  }

  if (reason && reason->empty())
    *reason = "Run claude to sign in";
  return std::nullopt;
}

std::optional<CodexCred> ResolveCodexCred() {
  auto raw = ReadFileUtf8(HomeDir() + L"\\.codex\\auth.json");
  if (!raw)
    return std::nullopt;
  auto tokens = JsonObject(*raw, "tokens");
  auto token = JsonString(tokens, "access_token");
  if (token.empty())
    token = JsonString(*raw, "access_token");
  if (token.empty())
    return std::nullopt;
  auto account = JsonString(tokens, "account_id");
  if (account.empty())
    account = JsonString(*raw, "account_id");
  return CodexCred{token, account};
}

}  // namespace cu
