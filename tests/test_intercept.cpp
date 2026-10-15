// Unit tests for the interception policy. These need no driver, no hardware and
// no DLL: they exercise the decision layer directly, which is where the rules
// from the specification live.

#include "doctest.h"

#include "../shim/include/DiCE.hpp"
#include "../shim/intercept.h"

#include <algorithm>

using namespace shim;

TEST_CASE("busy-channel MS fallback is restricted to Mongoose ISO15765 MS") {
  Config cfg;
  cfg.msFallback = true;
  for (const auto profile : {AdapterProfile::generic, AdapterProfile::autoDetect}) {
    cfg.adapterProfile = profile;
    CHECK_FALSE(shouldRetryMsConnect(cfg, ISO15765, PHYSICAL_CHANNEL, 125000,
                                     ERR_CHANNEL_IN_USE));
  }
  cfg.adapterProfile = AdapterProfile::mongoose;
  CHECK(shouldRetryMsConnect(cfg, ISO15765, PHYSICAL_CHANNEL, 125000,
                              ERR_CHANNEL_IN_USE));
  CHECK_FALSE(shouldRetryMsConnect(cfg, ISO15765, 0, 125000, ERR_CHANNEL_IN_USE));
  CHECK_FALSE(shouldRetryMsConnect(cfg, ISO15765, PHYSICAL_CHANNEL, 500000,
                                   ERR_CHANNEL_IN_USE));
  CHECK_FALSE(shouldRetryMsConnect(cfg, CAN, PHYSICAL_CHANNEL, 125000,
                                   ERR_CHANNEL_IN_USE));
  CHECK_FALSE(shouldRetryMsConnect(cfg, ISO15765, PHYSICAL_CHANNEL, 125000,
                                   ERR_DEVICE_NOT_CONNECTED));
  cfg.msFallback = false;
  CHECK_FALSE(shouldRetryMsConnect(cfg, ISO15765, PHYSICAL_CHANNEL, 125000,
                                   ERR_CHANNEL_IN_USE));
}

namespace {

// A configuration equivalent to the shipped INI, built by hand so the
// tests do not depend on a file.
Config defaultConfig() {
  Config c;
  c.intercept["ISO15765_SWDL_FABRICATE"] = Policy::absorb;
  c.intercept["CAN_XON_XOFF_STMIN"] = Policy::absorb;
  c.intercept["DICE_CAN_XON_XOFF_FILTER"] = Policy::absorb;
  c.intercept["CAN_XON_XOFF"] = Policy::reject;
  c.absorbUnknownVendorIds = true;
  return c;
}

} // namespace

TEST_CASE("standard entities are forwarded") {
  const Config c = defaultConfig();
  CHECK(policyFor(c, Category::config, DATA_RATE, "DATA_RATE", true) ==
        Policy::forward);
  CHECK(policyFor(c, Category::config, BIT_SAMPLE_POINT, "BIT_SAMPLE_POINT", true) ==
        Policy::forward);
  CHECK(policyFor(c, Category::ioctl, SET_CONFIG, "SET_CONFIG", true) ==
        Policy::forward);
}

TEST_CASE("the two DiCE vendor SET_CONFIG parameters are absorbed") {
  const Config c = defaultConfig();
  CHECK(policyFor(c, Category::config, j2534::dice::ISO15765_SWDL_FABRICATE,
                  "ISO15765_SWDL_FABRICATE", false) == Policy::absorb);
  CHECK(policyFor(c, Category::config, j2534::dice::CAN_XON_XOFF_STMIN,
                  "CAN_XON_XOFF_STMIN", false) == Policy::absorb);
}

TEST_CASE("the DiCE vendor protocol is rejected") {
  const Config c = defaultConfig();
  CHECK(policyFor(c, Category::protocol, j2534::dice::CAN_XON_XOFF_STMIN,
                  "CAN_XON_XOFF", false) == Policy::reject);
}

TEST_CASE("an unknown id is absorbed only inside the vendor range") {
  const Config c = defaultConfig();

  // Above the vendor base: whitelist-for-forward, so absorb.
  CHECK(policyFor(c, Category::config, 0x10000009UL, "UNKNOWN", false) ==
        Policy::absorb);
  CHECK(policyFor(c, Category::ioctl, 0x20000009UL, "UNKNOWN", false) ==
        Policy::absorb);

  // Below it: a J2534-2 entity this shim has no name for. Absorbing would
  // answer STATUS_NOERROR for a setting that was never applied.
  CHECK(policyFor(c, Category::config, ACTIVE_CHANNELS, "UNKNOWN", false) ==
        Policy::forward);
  CHECK(policyFor(c, Category::config, NON_VOLATILE_STORE_1, "UNKNOWN", false) ==
        Policy::forward);
  CHECK(policyFor(c, Category::ioctl, 0x30000UL, "UNKNOWN", false) == Policy::forward);
}

TEST_CASE("policies are scoped to one number space") {
  // 0x10000001 is simultaneously the CAN_XON_XOFF protocol and the
  // CAN_XON_XOFF_STMIN config parameter. Configuring one must not move the
  // other - that collision is the entire reason this shim exists, one level up.
  Config c = defaultConfig();
  c.intercept[numericKey(Category::config, 0x10000001UL)] = Policy::forward;

  CHECK(policyFor(c, Category::config, 0x10000001UL, "UNKNOWN", false) ==
        Policy::forward);
  CHECK(policyFor(c, Category::protocol, 0x10000001UL, "UNKNOWN", false) ==
        Policy::absorb);
  CHECK(policyFor(c, Category::ioctl, 0x10000001UL, "UNKNOWN", false) ==
        Policy::absorb);
}

TEST_CASE("numeric keys are canonical and category-qualified") {
  CHECK(numericKey(Category::config, 0x10000002UL) == "config:0x10000002");
  CHECK(numericKey(Category::protocol, 0x10000001UL) == "protocol:0x10000001");
  CHECK(numericKey(Category::flag, 0x20000000UL) == "flag:0x20000000");
}

TEST_CASE("a category-qualified name beats nothing and loses to a numeric key") {
  Config c;
  c.intercept["config:ISO15765_SWDL_FABRICATE"] = Policy::reject;
  CHECK(policyFor(c, Category::config, j2534::dice::ISO15765_SWDL_FABRICATE,
                  "ISO15765_SWDL_FABRICATE", false) == Policy::reject);

  c.intercept[numericKey(Category::config, j2534::dice::ISO15765_SWDL_FABRICATE)] =
      Policy::forward;
  CHECK(policyFor(c, Category::config, j2534::dice::ISO15765_SWDL_FABRICATE,
                  "ISO15765_SWDL_FABRICATE", false) == Policy::forward);
}

TEST_CASE("Connect flags keep PHYSICAL_CHANNEL and lose FUNC_ADDR") {
  const Config c = defaultConfig();
  unsigned long removed = 0;

  // Exactly what SDA sends for the MS channel, plus the DiCE vendor bit.
  const unsigned long flags =
      PHYSICAL_CHANNEL | CAN_29BIT_ID | j2534::dice::FUNC_ADDR;
  const unsigned long clean = filterConnectFlags(c, flags, &removed);

  CHECK(removed == 0x10000000UL);
  CHECK(clean == (PHYSICAL_CHANNEL | CAN_29BIT_ID));
  CHECK((clean & PHYSICAL_CHANNEL) != 0);
}

TEST_CASE("Connect flags without a vendor bit are untouched") {
  const Config c = defaultConfig();
  unsigned long removed = 0;
  const unsigned long flags = PHYSICAL_CHANNEL | CAN_29BIT_ID;
  CHECK(filterConnectFlags(c, flags, &removed) == flags);
  CHECK(removed == 0);
}

TEST_CASE("TxFlags lose every vendor bit, including 0x20000000") {
  // TxFlags is a different number space from Connect Flags: v04.04 defines no
  // TxFlags bit above 0x00800000, and neither vendor defines bit 29 - yet SDA
  // sets it on more than half of all messages in the reference trace.
  const Config c = defaultConfig();
  unsigned long removed = 0;

  const unsigned long txFlags = 0x20000000UL | ISO15765_FRAME_PAD; // seen live
  const unsigned long clean = filterTxFlags(c, txFlags, &removed);
  CHECK(removed == 0x20000000UL);
  CHECK(clean == ISO15765_FRAME_PAD);

  removed = 0;
  const unsigned long both = 0x20000000UL | 0x10000000UL | CAN_29BIT_ID;
  CHECK(filterTxFlags(c, both, &removed) == CAN_29BIT_ID);
  CHECK(removed == 0x30000000UL);
}

TEST_CASE("standard TxFlags survive") {
  const Config c = defaultConfig();
  unsigned long removed = 0;
  const unsigned long tx = CAN_29BIT_ID | ISO15765_FRAME_PAD | ISO15765_ADDR_TYPE;
  CHECK(filterTxFlags(c, tx, &removed) == tx);
  CHECK(removed == 0);
}

TEST_CASE("a TxFlags bit can be forwarded on purpose") {
  Config c = defaultConfig();
  c.intercept["flag:TX_BIT29"] = Policy::forward;
  unsigned long removed = 0;
  const unsigned long tx = 0x20000000UL | ISO15765_FRAME_PAD;
  CHECK(filterTxFlags(c, tx, &removed) == tx);
  CHECK(removed == 0);
}

TEST_CASE("a SET_CONFIG list is split by policy") {
  const Config c = defaultConfig();
  SCONFIG params[] = {
      {DATA_RATE, 500000},
      {j2534::dice::CAN_XON_XOFF_STMIN, 0},
      {BIT_SAMPLE_POINT, 68},
      {j2534::dice::ISO15765_SWDL_FABRICATE, 1},
  };
  SCONFIG_LIST list = {4, params};

  const ConfigFilter f = filterConfigList(c, &list);
  REQUIRE(f.forwarded.size() == 2);
  CHECK(f.forwarded[0].Parameter == DATA_RATE);
  CHECK(f.forwarded[1].Parameter == BIT_SAMPLE_POINT);
  REQUIRE(f.absorbed.size() == 2);
  CHECK(f.rejected.empty());
}

TEST_CASE("a rejected parameter is reported separately from an absorbed one") {
  Config c = defaultConfig();
  c.intercept["config:CAN_XON_XOFF_STMIN"] = Policy::reject;
  SCONFIG params[] = {{DATA_RATE, 500000}, {j2534::dice::CAN_XON_XOFF_STMIN, 0}};
  SCONFIG_LIST list = {2, params};

  const ConfigFilter f = filterConfigList(c, &list);
  CHECK(f.forwarded.size() == 1);
  CHECK(f.absorbed.empty());
  REQUIRE(f.rejected.size() == 1);
  CHECK(f.rejected[0].Parameter == j2534::dice::CAN_XON_XOFF_STMIN);
}

TEST_CASE("an empty or null list is handled") {
  const Config c = defaultConfig();
  CHECK(filterConfigList(c, nullptr).forwarded.empty());
  SCONFIG_LIST empty = {0, nullptr};
  CHECK(filterConfigList(c, &empty).forwarded.empty());
}

TEST_CASE("intercept keys that resolve to nothing are reported") {
  Config c;
  c.intercept["CAN_XON_XOFF"] = Policy::reject;          // valid bare name
  c.intercept["config:0x10000009"] = Policy::forward;    // valid numeric
  c.intercept["flag:TX_BIT29"] = Policy::absorb;         // valid scoped name
  c.intercept["CAN_XON_XOF"] = Policy::reject;           // typo
  c.intercept["cfg:DATA_RATE"] = Policy::forward;        // bad prefix
  c.intercept["config:NO_SUCH_PARAM"] = Policy::absorb;  // unknown in category

  const std::vector<std::string> bad = unknownInterceptKeys(c);
  REQUIRE(bad.size() == 3);
  CHECK(std::find(bad.begin(), bad.end(), "CAN_XON_XOF") != bad.end());
  CHECK(std::find(bad.begin(), bad.end(), "cfg:DATA_RATE") != bad.end());
  CHECK(std::find(bad.begin(), bad.end(), "config:NO_SUCH_PARAM") != bad.end());
}

TEST_CASE("protocol names disambiguate the DiCE overlap") {
  // 0x10000001 is CAN_XON_XOFF as a protocol and J1708_PS to DrewTech.
  CHECK(std::string(protocolName(j2534::dice::CAN_XON_XOFF_STMIN)) == "CAN_XON_XOFF");
  CHECK(std::string(protocolName(ISO15765)) == "ISO15765");
  CHECK(std::string(protocolName(ISO15765_PS)) == "ISO15765_PS");
  CHECK(!isStandardProtocol(j2534::dice::CAN_XON_XOFF_STMIN));
  CHECK(isStandardProtocol(ISO15765_PS));
}
