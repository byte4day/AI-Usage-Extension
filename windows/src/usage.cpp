#include "usage.hpp"
#include "auth.hpp"
#include "http.hpp"
#include "util.hpp"

#include <algorithm>
#include <cstdio>
#include <future>
#include <regex>
#include <vector>

namespace cu {

double ClampPct(double v) {
  if (!(v == v))
    return 0;
  return (std::min)(100.0, (std::max)(0.0, v));
}

static std::optional<double> PctFromMessage(const std::string& msg) {
  static const std::regex re(R"(([\d.]+)\s*%)");
  std::smatch m;
  if (std::regex_search(msg, m, re)) {
    try {
      return ClampPct(std::stod(m[1].str()));
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

static ProviderUsage NormalizeCursor(const std::string& json, bool showBilling) {
  ProviderUsage u;
  u.id = "cursor";
  u.tier = PlanLabel(JsonString(json, "membershipType"));
  u.isUnlimited = JsonBool(json, "isUnlimited").value_or(false);

  auto individual = JsonObject(json, "individualUsage");
  auto plan = JsonObject(individual, "plan");
  auto onDemand = JsonObject(individual, "onDemand");
  std::string end = JsonString(json, "billingCycleEnd");

  auto autoRaw = JsonNumber(plan, "autoPercentUsed");
  auto apiRaw = JsonNumber(plan, "apiPercentUsed");
  auto totalRaw = JsonNumber(plan, "totalPercentUsed");
  const auto autoMsg = PctFromMessage(JsonString(json, "autoModelSelectedDisplayMessage"));
  const auto apiMsg = PctFromMessage(JsonString(json, "namedModelSelectedDisplayMessage"));
  const bool hasAuto = autoRaw.has_value() || autoMsg.has_value();
  const bool hasApi = apiRaw.has_value() || apiMsg.has_value();
  double autoU = autoRaw ? ClampPct(*autoRaw)
                         : autoMsg.value_or(0);
  double apiU = apiRaw ? ClampPct(*apiRaw)
                       : apiMsg.value_or(0);
  double totalU = totalRaw ? ClampPct(*totalRaw) : (std::max)(autoU, apiU);

  u.a = {autoU, end, !hasAuto};
  u.b = {apiU, end, !hasApi};
  u.total = {totalU, end, !totalRaw.has_value() && !hasAuto && !hasApi};

  if (showBilling) {
    std::string parts;
    if (JsonBool(plan, "enabled").value_or(false)) {
      auto breakdown = JsonObject(plan, "breakdown");
      if (auto inc = JsonNumber(breakdown, "included")) {
        parts = CentsLabel(*inc);
        if (auto bonus = JsonNumber(breakdown, "bonus"); bonus && *bonus != 0)
          parts += " + " + CentsLabel(*bonus) + " bonus";
      }
    }
    if (JsonBool(onDemand, "enabled").value_or(false)) {
      if (!parts.empty())
        parts += " · ";
      auto used = JsonNumber(onDemand, "used").value_or(0);
      auto lim = JsonNumber(onDemand, "limit");
      parts += "on-demand " + CentsLabel(used) + " / " + (lim ? CentsLabel(*lim) : "∞");
    }
    u.billing = parts;
  }
  u.ok = true;
  return u;
}

static ProviderUsage NormalizeClaude(const std::string& json, const std::string& tier,
                                     bool showBilling) {
  ProviderUsage u;
  u.id = "claude";
  u.tier = PlanLabel(tier.empty() ? "claude" : tier);
  auto window = [](const std::string& obj) -> Pool {
    auto util = JsonNumber(obj, "utilization");
    if (obj.empty() || !util)
      return {0, {}, true};
    return {ClampPct(*util), JsonString(obj, "resets_at"), false};
  };
  u.a = window(JsonObject(json, "five_hour"));
  u.b = window(JsonObject(json, "seven_day"));
  double a = u.a.missing ? 0 : u.a.utilization;
  double b = u.b.missing ? 0 : u.b.utilization;
  u.total = {(std::max)(a, b), u.b.missing ? u.a.resetsAt : u.b.resetsAt, u.a.missing && u.b.missing};
  if (showBilling) {
    auto extra = JsonObject(json, "extra_usage");
    if (JsonBool(extra, "is_enabled").value_or(false)) {
      auto used = JsonNumber(extra, "used_credits");
      auto lim = JsonNumber(extra, "monthly_limit");
      if (used && lim)
        u.billing = "extra " + std::to_string(static_cast<int>(*used)) + " / " +
                    std::to_string(static_cast<int>(*lim));
      else
        u.billing = "extra " + std::to_string(static_cast<int>(
                                   ClampPct(JsonNumber(extra, "utilization").value_or(0)))) +
                    "%";
    }
  }
  u.ok = true;
  return u;
}

static ProviderUsage NormalizeCodex(const std::string& json, bool showBilling) {
  ProviderUsage u;
  u.id = "codex";
  u.tier = PlanLabel(JsonString(json, "plan_type").empty() ? "codex" : JsonString(json, "plan_type"));
  auto rate = JsonObject(json, "rate_limit");
  auto primary = JsonObject(rate, "primary_window");
  auto secondary = JsonObject(rate, "secondary_window");

  auto fromWin = [](const std::string& win) -> Pool {
    if (win.empty() || win == "null" || !JsonNumber(win, "used_percent"))
      return {0, {}, true};
    Pool p;
    p.utilization = ClampPct(*JsonNumber(win, "used_percent"));
    if (auto reset = JsonNumber(win, "reset_at"))
      p.resetsAt = "unix:" + std::to_string(static_cast<long long>(*reset));
    return p;
  };

  u.a = fromWin(primary);
  u.b = fromWin(secondary);
  u.total = {(std::max)(u.a.missing ? 0.0 : u.a.utilization, u.b.missing ? 0.0 : u.b.utilization),
             u.b.missing ? u.a.resetsAt : u.b.resetsAt, u.a.missing && u.b.missing};

  auto credits = JsonObject(json, "credits");
  u.isUnlimited = JsonBool(credits, "unlimited").value_or(false);
  if (showBilling && JsonBool(credits, "has_credits").value_or(false)) {
    if (u.isUnlimited)
      u.billing = "credits unlimited";
    else if (auto bal = JsonNumber(credits, "balance")) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "credits $%.2f", *bal);
      u.billing = buf;
    }
  }
  u.ok = true;
  return u;
}

static ProviderUsage FetchCursor(const Config& config) {
  ProviderUsage u;
  u.id = "cursor";
  auto token = ResolveCursorToken();
  if (!token) {
    u.error = "Login required";
    return u;
  }
  auto cookie = SessionCookieFromToken(*token);
  if (!cookie) {
    u.error = "Bad token";
    return u;
  }
  auto response = HttpGet(L"cursor.com", L"/api/usage-summary",
                          {{L"Cookie", L"WorkosCursorSessionToken=" + Utf8ToWide(*cookie)},
                           {L"User-Agent", L"ai-usage-widget"}},
                          config.proxyUrl);
  if (!response.ok()) {
    u.error = response.error.empty() ? HttpStatusLabel(response.status) : response.error;
    return u;
  }
  return NormalizeCursor(response.body, config.showBilling);
}

static ProviderUsage FetchClaude(const Config& config) {
  ProviderUsage u;
  u.id = "claude";
  std::string reason;
  auto cred = ResolveClaudeCred(config.proxyUrl, &reason);
  if (!cred) {
    u.error = reason.empty() ? "Run claude to sign in" : reason;
    return u;
  }
  auto request = [&](const std::string& token) {
    return HttpGet(L"api.anthropic.com", L"/api/oauth/usage",
                   {{L"Authorization", L"Bearer " + Utf8ToWide(token)},
                    {L"anthropic-beta", L"oauth-2025-04-20"},
                    {L"User-Agent", L"claude-code/2.1.72"},
                    {L"Content-Type", L"application/json"}},
                   config.proxyUrl);
  };

  auto response = request(cred->accessToken);
  // A stored token can be rejected before its recorded expiry (revoked, or the
  // clock is off). One forced refresh turns that into a working request instead
  // of a permanent "HTTP 401" until the user next runs the CLI by hand.
  if (!response.ok() && !cred->refreshed && (response.status == 401 || response.status == 403)) {
    std::string retryReason;
    if (auto renewed = ResolveClaudeCred(config.proxyUrl, &retryReason, /*force=*/true))
      response = request(renewed->accessToken);
    else if (!retryReason.empty()) {
      u.error = retryReason;
      return u;
    }
  }
  if (!response.ok()) {
    u.error = response.error.empty() ? HttpStatusLabel(response.status) : response.error;
    return u;
  }
  return NormalizeClaude(response.body, cred->subscriptionType, config.showBilling);
}

static ProviderUsage FetchCodex(const Config& config) {
  ProviderUsage u;
  u.id = "codex";
  auto cred = ResolveCodexCred();
  if (!cred) {
    u.error = "Login required";
    return u;
  }
  Headers headers = {
      {L"Authorization", L"Bearer " + Utf8ToWide(cred->accessToken)},
      {L"Accept", L"application/json"},
      {L"User-Agent", L"ai-usage-widget"},
  };
  if (!cred->accountId.empty())
    headers.emplace_back(L"ChatGPT-Account-Id", Utf8ToWide(cred->accountId));

  auto response = HttpGet(L"chatgpt.com", L"/backend-api/wham/usage", headers, config.proxyUrl);
  if (!response.ok()) {
    u.error = response.error.empty() ? HttpStatusLabel(response.status) : response.error;
    return u;
  }
  return NormalizeCodex(response.body, config.showBilling);
}

AllUsage FetchAllUsage(const Config& config) {
  AllUsage all;
  std::future<ProviderUsage> cursor, claude, codex;
  if (config.showCursor)
    cursor = std::async(std::launch::async, FetchCursor, std::cref(config));
  if (config.showClaude)
    claude = std::async(std::launch::async, FetchClaude, std::cref(config));
  if (config.showCodex)
    codex = std::async(std::launch::async, FetchCodex, std::cref(config));
  if (cursor.valid())
    all.cursor = cursor.get();
  if (claude.valid())
    all.claude = claude.get();
  if (codex.valid())
    all.codex = codex.get();
  return all;
}

const Pool* SelectPool(const ProviderUsage& data, const std::string& panelWindow) {
  // Resolve to null rather than to a pool flagged missing: callers read
  // utilization directly, so handing one back reports a confident 0%.
  const Pool* a = data.a.missing ? nullptr : &data.a;
  const Pool* b = data.b.missing ? nullptr : &data.b;
  const Pool* total = data.total.missing ? nullptr : &data.total;
  if (panelWindow == "api")
    return b ? b : (a ? a : total);
  if (panelWindow == "auto")
    return a ? a : (b ? b : total);
  if (panelWindow == "total")
    return total ? total : (a && b ? (a->utilization >= b->utilization ? a : b) : (a ? a : b));
  if (!a)
    return b ? b : total;
  if (!b)
    return a;
  return a->utilization >= b->utilization ? a : b;
}

const ProviderUsage* SelectProvider(const AllUsage& all, const Config& config) {
  auto usable = [&](const ProviderUsage& p) {
    return p.ok && (p.isUnlimited || SelectPool(p, config.panelWindow) != nullptr);
  };
  if (config.panelProvider == "cursor" && usable(all.cursor))
    return &all.cursor;
  if (config.panelProvider == "claude" && usable(all.claude))
    return &all.claude;
  if (config.panelProvider == "codex" && usable(all.codex))
    return &all.codex;

  const ProviderUsage* best = nullptr;
  double bestU = -1;
  for (const ProviderUsage* p : {&all.cursor, &all.claude, &all.codex}) {
    if (!usable(*p))
      continue;
    const Pool* pool = SelectPool(*p, config.panelWindow);
    double u = pool ? pool->utilization : 0;
    if (u > bestU) {
      bestU = u;
      best = p;
    }
  }
  return best;
}

}  // namespace cu
