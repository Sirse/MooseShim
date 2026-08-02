#pragma once

#include "config.h"
#include "include/J2534_v0404.h"

#include <string>
#include <vector>

namespace shim {

// Everything at or above this value is a vendor extension in J2534 v04.04.
// DiCE and DrewTech assign different meanings to the same numbers, so vendor
// ids must never be forwarded blindly.
constexpr unsigned long kVendorIdBase = 0x10000000UL;

// The same rule expressed as a bit mask, for flag words: bits 28..31 are the
// vendor range. A bit set there means different things to different vendors
// exactly like a vendor id does.
constexpr unsigned long kVendorFlagMask = 0xF0000000UL;

// J2534 keeps four independent number spaces, and the same numeric value means
// different things in each of them: 0x10000001 is the DiCE CAN_XON_XOFF
// *protocol* and also the CAN_XON_XOFF_STMIN *config parameter*; 0x10000000 is
// the FUNC_ADDR *flag bit* and also the DrewTech ISO15765_SIMULTANEOUS *config
// parameter*. Policies are therefore looked up per category, so configuring one
// of them cannot silently change the other three.
enum class Category { ioctl, config, protocol, flag };

// "ioctl" / "config" / "protocol" / "flag" - the prefix used in INI keys.
const char *categoryPrefix(Category c);

const char *ioctlName(unsigned long id);
const char *configParamName(unsigned long id);
const char *protocolName(unsigned long id);
const char *errorName(long code);

bool isStandardIoctl(unsigned long id);
bool isStandardConfigParam(unsigned long id);
bool isStandardProtocol(unsigned long id);

// The vendor bit DiCE puts into PassThruConnect Flags: FUNC_ADDR on DiCE ==
// SNIFF_MODE on DrewTech. PHYSICAL_CHANNEL (0x20000000) is standard J2534-2 and
// is used legitimately for the MS channel - in Connect flags it must survive.
constexpr unsigned long kFuncAddrBit = 0x10000000UL;

// Resolves the configured policy for one entity. Lookup order:
//   1. "<category>:0x10000002"  - a specific id in one number space
//   2. "<category>:NAME"        - a named entity in one number space
//   3. "NAME"                   - the same name in every number space
// `standard` decides the default when no key matches: standard entities are
// forwarded, vendor-range ones follow [intercept] absorb_unknown_vendor_ids.
Policy policyFor(const Config &cfg, Category cat, unsigned long id, const char *name,
                 bool standard);

// Canonical INI key for a numeric id in a category, e.g. "config:0x10000002".
std::string numericKey(Category cat, unsigned long id);

// Strips the vendor bit from PassThruConnect Flags. Only FUNC_ADDR is a
// candidate here; PHYSICAL_CHANNEL is standard and is never touched.
unsigned long filterConnectFlags(const Config &cfg, unsigned long flags,
                                 unsigned long *removed);

// Strips vendor bits from PASSTHRU_MSG::TxFlags. This is a *different* number
// space from Connect flags: J2534 v04.04 defines no TxFlags bit above
// 0x00800000, and neither DrewTech nor DiCE define bit 29 - yet SDA sets
// 0x20000000 there on more than half of the observed SDA messages.
// Every bit in the vendor range is removed unless explicitly configured to
// forward.
unsigned long filterTxFlags(const Config &cfg, unsigned long flags,
                            unsigned long *removed);

// Result of filtering a SET_CONFIG / GET_CONFIG list.
struct ConfigFilter {
  std::vector<SCONFIG> forwarded; // parameters that may reach the target
  std::vector<SCONFIG> absorbed;  // parameters answered by the shim
  std::vector<SCONFIG> rejected;  // parameters with policy=reject
};

ConfigFilter filterConfigList(const Config &cfg, const SCONFIG_LIST *list);

// [intercept] keys that resolve to nothing. A typo used to be silently
// accepted, which meant a policy the operator believed was in force was not.
std::vector<std::string> unknownInterceptKeys(const Config &cfg);

} // namespace shim
