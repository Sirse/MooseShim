#pragma once

#include <map>
#include <string>
#include <vector>

namespace shim {

// Policy applied to a vendor-specific ioctl / config parameter / protocol.
enum class Policy {
  forward, // pass through to the target J2534 driver unchanged
  absorb,  // never reaches the target; shim answers STATUS_NOERROR
  reject   // never reaches the target; shim answers an error code
};

enum class AdapterProfile {
  autoDetect, // conservative fallbacks, enabled only after a target refusal
  generic,    // strict J2534 v04.04; no vendor-behaviour normalisation
  mongoose    // DrewTech MongoosePro JLR compatibility
};

enum class D2Route {
  reject,
  autoDetect,
  can,
  canPs
};

struct Config {
  // [target]
  std::string library; // full path to the real J2534 DLL (e.g. Mongoose)
  AdapterProfile adapterProfile = AdapterProfile::autoDetect;

  // What to hand the target driver as PassThruOpen's pName. SDA passes the
  // name from its registry entry (the shim's own name); DiCE ignored that, but
  // the DrewTech driver treats pName as a device selector and answers
  // ERR_DEVICE_NOT_CONNECTED. J2534 v04.04 says NULL means "first available
  // device", which is what the shim sends unless a name is configured here.
  std::string deviceName;
  bool forwardDeviceName = false; // true: pass SDA's string through unchanged

  // [channel]
  // SDA opens the MS bus as ISO15765 + PHYSICAL_CHANNEL. Adapters that have no
  // second physical CAN answer ERR_INVALID_FLAGS, but can still reach the MS
  // pins through the J2534-2 pin-switched protocols. When this is on, such a
  // failure is retried as ISO15765_PS/CAN_PS with J1962_PINS, which is the
  // route VolvoTools uses (Common/src/Util.cpp, openUDSChannel).
  bool msFallback = true;
  unsigned long msPins = 0x030B; // CAN low on pin 3, CAN high on pin 11

  // Experimental Volvo D2 translation. It is deliberately disabled unless
  // the operator selects a route explicitly.
  D2Route d2Route = D2Route::reject;
  unsigned long d2MsPins = 0x030B;
  bool d2IgnoreIntermediateTxAck = true;
  // Set when the environment overrode d2Route. This is the only way to reach a
  // vehicle-facing experimental path without editing the INI, so the override
  // is announced in the log instead of taking effect silently.
  std::string d2RouteOverride;

  // CLEAR_RX_BUFFER is a standard v04.04 ioctl that DiCE implements and
  // MongoosePro JLR answers ERR_FAILED to. SDA calls it before every request,
  // and without it the queue still holds the transmit indication of the
  // previous write, so SDA's first read after each write returns its own echo
  // instead of the ECU's answer. When this is on, a refusal is turned into a
  // drain: read until the queue is empty, then report success.
  bool emulateClearRxBuffer = true;

  // Functional 0x7DF wake/reset frames are best-effort broadcasts. DiCE reports
  // their submission as successful even when no ECU acknowledges a particular
  // bus; Mongoose instead waits and returns ERR_TIMEOUT, which makes SDA abort
  // before trying the bus where the ECU actually lives.
  bool tolerateUnackedFunctionalBroadcasts = false;

  // A blocking J2534 read that expires without receiving anything is a timeout.
  // Some DrewTech drivers return ERR_BUFFER_EMPTY instead; SDA treats that as
  // a protocol failure rather than the ordinary "ECU did not answer" outcome.
  // Non-blocking reads (Timeout == 0) retain ERR_BUFFER_EMPTY.
  bool normalizeEmptyBlockingRead = true;

  // SDA can close a FLOW_CONTROL_FILTER using the other bus channel. J2534
  // filter ids are channel-scoped, so repair that call only when exactly one
  // active filter with the supplied id exists on the same device.
  bool repairCrossChannelFilterStop = false;

  // [log]
  std::string logPath;
  std::string logLevel = "debug";

  // [intercept] key -> policy. A key is either a bare entity name
  // ("CAN_XON_XOFF"), which applies in every number space, or a name/id scoped
  // to one space ("config:0x10000002", "protocol:CAN_XON_XOFF"). Keys that
  // resolve to nothing are reported through `warnings` rather than silently
  // ignored - see intercept.h, Category.
  std::map<std::string, Policy> intercept;

  // Problems found while parsing. The logger does not exist yet at that point,
  // so they are collected here and written once it is open. A typo in this file
  // used to be invisible, which meant a policy the operator believed was in
  // force silently was not.
  std::vector<std::string> warnings;

  // If true (default), any SET_CONFIG parameter or Ioctl id in the vendor
  // range (>= 0x10000000) that is not explicitly listed as "forward" is
  // absorbed. This is the "whitelist for forwarding" rule: vendor ids collide
  // between DiCE and DrewTech, so forwarding an unknown one calls a foreign
  // function on the target device.
  bool absorbUnknownVendorIds = true;

  // Loads the INI beside the DLL. A missing file is not fatal: defaults apply
  // and the error text is placed in `error`.
  static Config load(const std::string &iniPath, std::string *error);
};

// Directory of the currently loaded shim DLL, with trailing backslash.
std::string moduleDirectory(void *hModule);

// Expands %VAR% environment references in a path.
std::string expandEnv(const std::string &s);

} // namespace shim
