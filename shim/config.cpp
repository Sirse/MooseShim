#include "config.h"

#include "product.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <windows.h>

namespace shim {
namespace {

std::string trim(const std::string &s) {
  const char *ws = " \t\r\n";
  const size_t b = s.find_first_not_of(ws);
  if (b == std::string::npos)
    return {};
  const size_t e = s.find_last_not_of(ws);
  return s.substr(b, e - b + 1);
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return static_cast<char>(::tolower(c)); });
  return s;
}

// Unrecognised words fall back to `absorb`, the safe direction: a typo can turn
// "forward" into "do not touch the target", never the other way round.
bool parsePolicy(const std::string &v, Policy *out) {
  const std::string s = lower(v);
  *out = Policy::absorb;
  if (s == "forward") {
    *out = Policy::forward;
    return true;
  }
  if (s == "reject") {
    *out = Policy::reject;
    return true;
  }
  return s == "absorb";
}

bool isHex(const std::string &s) {
  return s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X');
}

// Canonicalises the numeric part of an intercept key so it matches what
// intercept.cpp builds: "config:0x10002" -> "config:0x00010002". Names and
// unparseable values are returned unchanged; a bare numeric key (no category
// prefix) is rejected by the caller, because the same number means different
// things in each of J2534's four number spaces.
std::string canonicalKey(const std::string &key) {
  const size_t colon = key.find(':');
  if (colon == std::string::npos)
    return key;
  const std::string prefix = key.substr(0, colon);
  const std::string rest = key.substr(colon + 1);
  if (!isHex(rest))
    return key;
  char *end = nullptr;
  const unsigned long id = strtoul(rest.c_str(), &end, 16);
  if (!end || *end != '\0')
    return key;
  char buf[32];
  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s:0x%08lX", prefix.c_str(), id);
  return buf;
}

bool parseBool(const std::string &v) {
  const std::string s = lower(v);
  return s == "1" || s == "true" || s == "yes" || s == "on";
}

bool parseAdapterProfile(const std::string &v, AdapterProfile *out) {
  const std::string s = lower(v);
  if (s == "auto") {
    *out = AdapterProfile::autoDetect;
    return true;
  }
  if (s == "generic") {
    *out = AdapterProfile::generic;
    return true;
  }
  if (s == "mongoose") {
    *out = AdapterProfile::mongoose;
    return true;
  }
  return false;
}

bool parseD2Route(const std::string &v, D2Route *out) {
  const std::string s = lower(v);
  if (s == "reject") {
    *out = D2Route::reject;
    return true;
  }
  if (s == "auto") {
    *out = D2Route::autoDetect;
    return true;
  }
  if (s == "can") {
    *out = D2Route::can;
    return true;
  }
  if (s == "can_ps") {
    *out = D2Route::canPs;
    return true;
  }
  return false;
}

void applyAdapterProfile(Config *cfg) {
  if (cfg->adapterProfile == AdapterProfile::generic) {
    cfg->msFallback = false;
    cfg->emulateClearRxBuffer = false;
    cfg->tolerateUnackedFunctionalBroadcasts = false;
    cfg->normalizeEmptyBlockingRead = false;
    cfg->repairCrossChannelFilterStop = false;
    return;
  }

  // Both auto and mongoose retain the conservative compatibility behaviour:
  // every workaround is gated by a specific target refusal/result.  The
  // explicit mongoose name documents a qualified driver; auto is suitable
  // for an unknown driver and can be narrowed with the legacy per-feature
  // overrides below.
  cfg->msFallback = true;
  cfg->emulateClearRxBuffer = true;
  cfg->tolerateUnackedFunctionalBroadcasts =
      cfg->adapterProfile == AdapterProfile::mongoose;
  cfg->normalizeEmptyBlockingRead = true;
  cfg->repairCrossChannelFilterStop =
      cfg->adapterProfile == AdapterProfile::mongoose;
}

} // namespace

std::string expandEnv(const std::string &s) {
  if (s.find('%') == std::string::npos)
    return s;
  char buf[MAX_PATH * 2];
  const DWORD n = ExpandEnvironmentStringsA(s.c_str(), buf, sizeof(buf));
  if (n == 0 || n > sizeof(buf))
    return s;
  return std::string(buf, n - 1);
}

std::string moduleDirectory(void *hModule) {
  char path[MAX_PATH] = {};
  const DWORD n = GetModuleFileNameA(static_cast<HMODULE>(hModule), path, MAX_PATH);
  if (n == 0)
    return {};
  std::string p(path, n);
  const size_t slash = p.find_last_of("\\/");
  return slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
}

Config Config::load(const std::string &iniPath, std::string *error) {
  Config cfg;
  bool msFallbackSet = false;
  bool emulateClearSet = false;
  bool tolerateUnackedSet = false;
  bool normalizeEmptySet = false;
  bool repairFilterStopSet = false;
  // Defaults matching the specification: both DiCE vendor SET_CONFIG
  // parameters are absorbed.
  cfg.intercept["ISO15765_SWDL_FABRICATE"] = Policy::absorb;
  cfg.intercept["CAN_XON_XOFF_STMIN"] = Policy::absorb;

  // <NAME>_TARGET overrides [target] library (see product.h). It exists so the automated
  // tests can aim the shim at the mock driver while the INI keeps pointing at
  // the real adapter.
  char envTarget[MAX_PATH * 2];
  const DWORD envLen = GetEnvironmentVariableA(SHIM_ENV_TARGET, envTarget,
                                               sizeof(envTarget));
  const std::string overrideLibrary =
      (envLen > 0 && envLen < sizeof(envTarget)) ? std::string(envTarget, envLen)
                                                 : std::string();

  FILE *f = nullptr;
  if (fopen_s(&f, iniPath.c_str(), "rb") != 0 || !f) {
    if (!overrideLibrary.empty())
      cfg.library = overrideLibrary;
    if (error)
      *error = "cannot open " + iniPath;
    return cfg;
  }

  std::string section;
  char line[1024];
  while (fgets(line, sizeof(line), f)) {
    std::string s = trim(line);
    if (s.empty() || s[0] == ';' || s[0] == '#')
      continue;
    if (s.front() == '[' && s.back() == ']') {
      section = lower(trim(s.substr(1, s.size() - 2)));
      continue;
    }
    const size_t eq = s.find('=');
    if (eq == std::string::npos)
      continue;
    const std::string key = trim(s.substr(0, eq));
    const std::string value = trim(s.substr(eq + 1));

    if (section == "target") {
      const std::string k = lower(key);
      if (k == "library")
        cfg.library = expandEnv(value);
      else if (k == "adapter_profile") {
        if (!parseAdapterProfile(value, &cfg.adapterProfile))
          cfg.warnings.push_back("[target] adapter_profile: unknown value [" +
                                 value + "] (expected auto|generic|mongoose)");
      }
      else if (k == "device_name")
        cfg.deviceName = value;
      else if (k == "forward_device_name")
        cfg.forwardDeviceName = parseBool(value);
      else
        cfg.warnings.push_back("unknown key [target] " + key + " - ignored");
    } else if (section == "channel") {
      const std::string k = lower(key);
      if (k == "ms_fallback") {
        cfg.msFallback = parseBool(value);
        msFallbackSet = true;
      }
      else if (k == "ms_pins")
        cfg.msPins = strtoul(value.c_str(), nullptr, 0);
      else if (k == "d2_route") {
        if (!parseD2Route(value, &cfg.d2Route))
          cfg.warnings.push_back("[channel] d2_route: unknown value [" +
                                 value + "] (expected reject|auto|can|can_ps)");
      } else if (k == "d2_ms_pins") {
        cfg.d2MsPins = strtoul(value.c_str(), nullptr, 0);
      } else if (k == "d2_ignore_intermediate_tx_ack") {
        cfg.d2IgnoreIntermediateTxAck = parseBool(value);
      }
      else if (k == "emulate_clear_rx_buffer") {
        cfg.emulateClearRxBuffer = parseBool(value);
        emulateClearSet = true;
      } else if (k == "tolerate_unacked_functional_broadcasts") {
        cfg.tolerateUnackedFunctionalBroadcasts = parseBool(value);
        tolerateUnackedSet = true;
      } else if (k == "normalize_empty_blocking_read") {
        cfg.normalizeEmptyBlockingRead = parseBool(value);
        normalizeEmptySet = true;
      } else if (k == "repair_cross_channel_filter_stop") {
        cfg.repairCrossChannelFilterStop = parseBool(value);
        repairFilterStopSet = true;
      }
      else
        cfg.warnings.push_back("unknown key [channel] " + key + " - ignored");
    } else if (section == "log") {
      const std::string k = lower(key);
      if (k == "path")
        cfg.logPath = expandEnv(value);
      else if (k == "level")
        cfg.logLevel = lower(value);
      else
        cfg.warnings.push_back("unknown key [log] " + key + " - ignored");
    } else if (section == "intercept") {
      if (lower(key) == "absorb_unknown_vendor_ids") {
        cfg.absorbUnknownVendorIds = parseBool(value);
        continue;
      }
      Policy policy = Policy::absorb;
      if (!parsePolicy(value, &policy))
        cfg.warnings.push_back("[intercept] " + key + ": unknown policy [" + value +
                               "], using absorb (expected forward|absorb|reject)");
      if (isHex(key)) {
        cfg.warnings.push_back(
            "[intercept] " + key +
            ": a numeric key needs a category prefix (config:/ioctl:/protocol:/"
            "flag:), because the same id means different things in each - "
            "ignored");
        continue;
      }
      cfg.intercept[canonicalKey(key)] = policy;
    } else if (!section.empty()) {
      cfg.warnings.push_back("unknown key [" + section + "] " + key + " - ignored");
    }
  }
  fclose(f);
  char envD2Route[32];
  const DWORD envD2RouteLen = GetEnvironmentVariableA(
      SHIM_ENV_D2_ROUTE, envD2Route, sizeof(envD2Route));
  if (envD2RouteLen > 0 && envD2RouteLen < sizeof(envD2Route)) {
    const std::string value(envD2Route, envD2RouteLen);
    if (!parseD2Route(value, &cfg.d2Route))
      cfg.warnings.push_back(std::string(SHIM_ENV_D2_ROUTE) +
                             ": unknown value [" + value + "]");
    else
      cfg.d2RouteOverride = value;
  }
  const bool explicitMsFallback = msFallbackSet;
  const bool explicitEmulateClear = emulateClearSet;
  const bool explicitTolerateUnacked = tolerateUnackedSet;
  const bool explicitNormalizeEmpty = normalizeEmptySet;
  const bool msFallback = cfg.msFallback;
  const bool emulateClear = cfg.emulateClearRxBuffer;
  const bool tolerateUnacked = cfg.tolerateUnackedFunctionalBroadcasts;
  const bool normalizeEmpty = cfg.normalizeEmptyBlockingRead;
  const bool repairFilterStop = cfg.repairCrossChannelFilterStop;
  applyAdapterProfile(&cfg);
  if (explicitMsFallback)
    cfg.msFallback = msFallback;
  if (explicitEmulateClear)
    cfg.emulateClearRxBuffer = emulateClear;
  if (explicitTolerateUnacked)
    cfg.tolerateUnackedFunctionalBroadcasts = tolerateUnacked;
  if (explicitNormalizeEmpty)
    cfg.normalizeEmptyBlockingRead = normalizeEmpty;
  if (repairFilterStopSet)
    cfg.repairCrossChannelFilterStop = repairFilterStop;
  if (!overrideLibrary.empty())
    cfg.library = overrideLibrary;
  return cfg;
}

} // namespace shim
