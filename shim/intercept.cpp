#include "intercept.h"

#include "include/DiCE.hpp"

#include <cstdio>

namespace shim {
namespace {

struct NameEntry {
  unsigned long id;
  const char *name;
};

// Standard v04.04 (plus the J2534-2 ids SDA may legitimately use).
const NameEntry kIoctls[] = {
    {GET_CONFIG, "GET_CONFIG"},
    {SET_CONFIG, "SET_CONFIG"},
    {READ_VBATT, "READ_VBATT"},
    {FIVE_BAUD_INIT, "FIVE_BAUD_INIT"},
    {FAST_INIT, "FAST_INIT"},
    {CLEAR_TX_BUFFER, "CLEAR_TX_BUFFER"},
    {CLEAR_RX_BUFFER, "CLEAR_RX_BUFFER"},
    {CLEAR_PERIODIC_MSGS, "CLEAR_PERIODIC_MSGS"},
    {CLEAR_MSG_FILTERS, "CLEAR_MSG_FILTERS"},
    {CLEAR_FUNCT_MSG_LOOKUP_TABLE, "CLEAR_FUNCT_MSG_LOOKUP_TABLE"},
    {ADD_TO_FUNCT_MSG_LOOKUP_TABLE, "ADD_TO_FUNCT_MSG_LOOKUP_TABLE"},
    {DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE, "DELETE_FROM_FUNCT_MSG_LOOKUP_TABLE"},
    {READ_PROG_VOLTAGE, "READ_PROG_VOLTAGE"},
    {SW_CAN_HS, "SW_CAN_HS"},
    {SW_CAN_NS, "SW_CAN_NS"},
    {GET_DEVICE_CONFIG, "GET_DEVICE_CONFIG"},
    {SET_DEVICE_CONFIG, "SET_DEVICE_CONFIG"},
};

// Vendor ioctl ids observed in, or reachable from, the SDA code paths. The
// DiCE meaning is authoritative for naming here; the DrewTech collision is
// noted because it is exactly what must not be triggered.
const NameEntry kVendorIoctls[] = {
    {j2534::dice::DICE_CAN_XON_XOFF_FILTER, "DICE_CAN_XON_XOFF_FILTER"}, // == DT_READ_DIO
    {CAN_XON_XOFF_FILTER_ACTIVE, "DICE_CAN_XON_XOFF_FILTER_ACTIVE"},     // == DT_WRITE_DIO
};

const NameEntry kConfigParams[] = {
    {DATA_RATE, "DATA_RATE"},
    {LOOPBACK, "LOOPBACK"},
    {NODE_ADDRESS, "NODE_ADDRESS"},
    {NETWORK_LINE, "NETWORK_LINE"},
    {P1_MIN, "P1_MIN"},
    {P1_MAX, "P1_MAX"},
    {P2_MIN, "P2_MIN"},
    {P2_MAX, "P2_MAX"},
    {P3_MIN, "P3_MIN"},
    {P3_MAX, "P3_MAX"},
    {P4_MIN, "P4_MIN"},
    {P4_MAX, "P4_MAX"},
    {W0, "W0"},
    {W1, "W1"},
    {W2, "W2"},
    {W3, "W3"},
    {W4, "W4"},
    {W5, "W5"},
    {TIDLE, "TIDLE"},
    {TINIL, "TINIL"},
    {TWUP, "TWUP"},
    {PARITY, "PARITY"},
    {BIT_SAMPLE_POINT, "BIT_SAMPLE_POINT"},
    {SYNC_JUMP_WIDTH, "SYNC_JUMP_WIDTH"},
    {T1_MAX, "T1_MAX"},
    {T2_MAX, "T2_MAX"},
    {T3_MAX, "T3_MAX"},
    {T4_MAX, "T4_MAX"},
    {T5_MAX, "T5_MAX"},
    {ISO15765_BS, "ISO15765_BS"},
    {ISO15765_STMIN, "ISO15765_STMIN"},
    {DATA_BITS, "DATA_BITS"},
    {FIVE_BAUD_MOD, "FIVE_BAUD_MOD"},
    {BS_TX, "BS_TX"},
    {STMIN_TX, "STMIN_TX"},
    {ISO15765_WFT_MAX, "ISO15765_WFT_MAX"},
    {CAN_MIXED_FORMAT, "CAN_MIXED_FORMAT"},
    {J1962_PINS, "J1962_PINS"},
    {SW_CAN_HS_DATA_RATE, "SW_CAN_HS_DATA_RATE"},
    {SW_CAN_SPEEDCHANGE_ENABLE, "SW_CAN_SPEEDCHANGE_ENABLE"},
    {SW_CAN_RES_SWITCH, "SW_CAN_RES_SWITCH"},
};

// Vendor SET_CONFIG parameters. Both collide with DrewTech ids:
//   0x10000001 CAN_XON_XOFF_STMIN      == DT_ISO15765_PAD_BYTE
//   0x10000002 ISO15765_SWDL_FABRICATE == DT_FILTER_FREQ
const NameEntry kVendorConfigParams[] = {
    {j2534::dice::CAN_XON_XOFF_STMIN, "CAN_XON_XOFF_STMIN"},
    {j2534::dice::ISO15765_SWDL_FABRICATE, "ISO15765_SWDL_FABRICATE"},
};

const NameEntry kProtocols[] = {
    {J1850VPW, "J1850VPW"}, {J1850PWM, "J1850PWM"},   {ISO9141, "ISO9141"},
    {ISO14230, "ISO14230"}, {CAN, "CAN"},             {ISO15765, "ISO15765"},
    {CAN_PS, "CAN_PS"},     {ISO15765_PS, "ISO15765_PS"},
    {SW_CAN_PS, "SW_CAN_PS"}, {SW_ISO15765_PS, "SW_ISO15765_PS"},
};

const NameEntry kVendorProtocols[] = {
    {j2534::dice::CAN_XON_XOFF_STMIN, "CAN_XON_XOFF"}, // 0x10000001 as a ProtocolID
};

// Bits 28..31 of a flag word. Connect Flags and TxFlags are different number
// spaces, so the names are per-space: 0x20000000 is the standard
// PHYSICAL_CHANNEL in Connect Flags, but nothing at all in TxFlags.
const NameEntry kConnectFlagBits[] = {
    {0x10000000UL, "FUNC_ADDR"},
    {0x20000000UL, "PHYSICAL_CHANNEL"},
    {0x40000000UL, "CONNECT_BIT30"},
    {0x80000000UL, "CONNECT_BIT31"},
};

const NameEntry kTxFlagBits[] = {
    {0x10000000UL, "TX_FUNC_ADDR"},
    {0x20000000UL, "TX_BIT29"},
    {0x40000000UL, "TX_BIT30"},
    {0x80000000UL, "TX_BIT31"},
};

const NameEntry kErrors[] = {
    {STATUS_NOERROR, "STATUS_NOERROR"},
    {ERR_NOT_SUPPORTED, "ERR_NOT_SUPPORTED"},
    {ERR_INVALID_CHANNEL_ID, "ERR_INVALID_CHANNEL_ID"},
    {ERR_INVALID_PROTOCOL_ID, "ERR_INVALID_PROTOCOL_ID"},
    {ERR_NULL_PARAMETER, "ERR_NULL_PARAMETER"},
    {ERR_INVALID_IOCTL_VALUE, "ERR_INVALID_IOCTL_VALUE"},
    {ERR_INVALID_FLAGS, "ERR_INVALID_FLAGS"},
    {ERR_FAILED, "ERR_FAILED"},
    {ERR_DEVICE_NOT_CONNECTED, "ERR_DEVICE_NOT_CONNECTED"},
    {ERR_TIMEOUT, "ERR_TIMEOUT"},
    {ERR_INVALID_MSG, "ERR_INVALID_MSG"},
    {ERR_INVALID_TIME_INTERVAL, "ERR_INVALID_TIME_INTERVAL"},
    {ERR_EXCEEDED_LIMIT, "ERR_EXCEEDED_LIMIT"},
    {ERR_INVALID_MSG_ID, "ERR_INVALID_MSG_ID"},
    {ERR_DEVICE_IN_USE, "ERR_DEVICE_IN_USE"},
    {ERR_INVALID_IOCTL_ID, "ERR_INVALID_IOCTL_ID"},
    {ERR_BUFFER_EMPTY, "ERR_BUFFER_EMPTY"},
    {ERR_BUFFER_FULL, "ERR_BUFFER_FULL"},
    {ERR_BUFFER_OVERFLOW, "ERR_BUFFER_OVERFLOW"},
    {ERR_PIN_INVALID, "ERR_PIN_INVALID"},
    {ERR_CHANNEL_IN_USE, "ERR_CHANNEL_IN_USE"},
    {ERR_MSG_PROTOCOL_ID, "ERR_MSG_PROTOCOL_ID"},
    {ERR_INVALID_FILTER_ID, "ERR_INVALID_FILTER_ID"},
    {ERR_NO_FLOW_CONTROL, "ERR_NO_FLOW_CONTROL"},
    {ERR_NOT_UNIQUE, "ERR_NOT_UNIQUE"},
    {ERR_INVALID_BAUDRATE, "ERR_INVALID_BAUDRATE"},
    {ERR_INVALID_DEVICE_ID, "ERR_INVALID_DEVICE_ID"},
};

template <size_t N> const char *lookup(const NameEntry (&table)[N], unsigned long id) {
  for (size_t i = 0; i < N; ++i)
    if (table[i].id == id)
      return table[i].name;
  return nullptr;
}

template <size_t N> bool hasName(const NameEntry (&table)[N], const std::string &name) {
  for (size_t i = 0; i < N; ++i)
    if (name == table[i].name)
      return true;
  return false;
}

bool knownName(Category cat, const std::string &name) {
  switch (cat) {
  case Category::ioctl:
    return hasName(kIoctls, name) || hasName(kVendorIoctls, name);
  case Category::config:
    return hasName(kConfigParams, name) || hasName(kVendorConfigParams, name);
  case Category::protocol:
    return hasName(kProtocols, name) || hasName(kVendorProtocols, name);
  case Category::flag:
    return hasName(kConnectFlagBits, name) || hasName(kTxFlagBits, name);
  }
  return false;
}

const Category kAllCategories[] = {Category::ioctl, Category::config,
                                   Category::protocol, Category::flag};

// A flag word is filtered bit by bit, because each bit is an independent
// vendor extension with its own policy.
unsigned long filterFlagBits(const Config &cfg, unsigned long flags,
                             unsigned long candidates, const NameEntry *names,
                             size_t nameCount, unsigned long *removed) {
  unsigned long strip = 0;
  for (unsigned long bit = 0x10000000UL; bit != 0; bit <<= 1) {
    if (!(flags & bit) || !(candidates & bit))
      continue;
    const char *name = "UNKNOWN";
    for (size_t i = 0; i < nameCount; ++i)
      if (names[i].id == bit)
        name = names[i].name;
    if (policyFor(cfg, Category::flag, bit, name, false) != Policy::forward)
      strip |= bit;
  }
  if (removed)
    *removed = strip;
  return flags & ~strip;
}

} // namespace

const char *categoryPrefix(Category c) {
  switch (c) {
  case Category::ioctl:
    return "ioctl";
  case Category::config:
    return "config";
  case Category::protocol:
    return "protocol";
  case Category::flag:
    return "flag";
  }
  return "?";
}

const char *ioctlName(unsigned long id) {
  if (const char *n = lookup(kIoctls, id))
    return n;
  if (const char *n = lookup(kVendorIoctls, id))
    return n;
  return "UNKNOWN";
}

const char *configParamName(unsigned long id) {
  if (const char *n = lookup(kConfigParams, id))
    return n;
  if (const char *n = lookup(kVendorConfigParams, id))
    return n;
  return "UNKNOWN";
}

const char *protocolName(unsigned long id) {
  if (const char *n = lookup(kVendorProtocols, id))
    return n;
  if (const char *n = lookup(kProtocols, id))
    return n;
  return "UNKNOWN";
}

const char *errorName(long code) {
  if (const char *n = lookup(kErrors, static_cast<unsigned long>(code)))
    return n;
  return "UNKNOWN";
}

bool isStandardIoctl(unsigned long id) { return lookup(kIoctls, id) != nullptr; }

bool isStandardConfigParam(unsigned long id) {
  return lookup(kConfigParams, id) != nullptr;
}

bool isStandardProtocol(unsigned long id) {
  return lookup(kProtocols, id) != nullptr;
}

std::string numericKey(Category cat, unsigned long id) {
  char buf[32];
  _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s:0x%08lX", categoryPrefix(cat), id);
  return buf;
}

Policy policyFor(const Config &cfg, Category cat, unsigned long id, const char *name,
                 bool standard) {
  auto it = cfg.intercept.find(numericKey(cat, id));
  if (it != cfg.intercept.end())
    return it->second;

  it = cfg.intercept.find(std::string(categoryPrefix(cat)) + ":" + name);
  if (it != cfg.intercept.end())
    return it->second;

  it = cfg.intercept.find(name);
  if (it != cfg.intercept.end())
    return it->second;

  if (standard)
    return Policy::forward;

  // The absorb default is the "whitelist for forwarding" rule from the
  // specification, and it applies to the vendor range only. An id below
  // 0x10000000 that is merely absent from the tables here is a standard or
  // J2534-2 entity this shim does not know by name - absorbing it would answer
  // STATUS_NOERROR for a setting that was never applied.
  if (id < kVendorIdBase)
    return Policy::forward;
  return cfg.absorbUnknownVendorIds ? Policy::absorb : Policy::forward;
}

unsigned long filterConnectFlags(const Config &cfg, unsigned long flags,
                                 unsigned long *removed) {
  // Only FUNC_ADDR is a candidate. PHYSICAL_CHANNEL (0x20000000) is standard
  // J2534-2 and is how SDA selects the MS channel.
  return filterFlagBits(cfg, flags, kFuncAddrBit, kConnectFlagBits,
                        sizeof(kConnectFlagBits) / sizeof(kConnectFlagBits[0]),
                        removed);
}

unsigned long filterTxFlags(const Config &cfg, unsigned long flags,
                            unsigned long *removed) {
  // Every vendor bit is a candidate here: TxFlags has no standard meaning above
  // 0x00800000 in v04.04.
  return filterFlagBits(cfg, flags, kVendorFlagMask, kTxFlagBits,
                        sizeof(kTxFlagBits) / sizeof(kTxFlagBits[0]), removed);
}

ConfigFilter filterConfigList(const Config &cfg, const SCONFIG_LIST *list) {
  ConfigFilter result;
  if (!list || !list->ConfigPtr)
    return result;

  for (uint32_t i = 0; i < list->NumOfParams; ++i) {
    const SCONFIG &p = list->ConfigPtr[i];
    const bool standard = isStandardConfigParam(p.Parameter);
    const Policy policy = policyFor(cfg, Category::config, p.Parameter,
                                    configParamName(p.Parameter), standard);
    switch (policy) {
    case Policy::forward:
      result.forwarded.push_back(p);
      break;
    case Policy::absorb:
      result.absorbed.push_back(p);
      break;
    case Policy::reject:
      result.rejected.push_back(p);
      break;
    }
  }
  return result;
}

std::vector<std::string> unknownInterceptKeys(const Config &cfg) {
  std::vector<std::string> bad;
  for (const auto &entry : cfg.intercept) {
    const std::string &key = entry.first;
    const size_t colon = key.find(':');

    if (colon == std::string::npos) {
      bool ok = false;
      for (Category c : kAllCategories)
        ok = ok || knownName(c, key);
      if (!ok)
        bad.push_back(key);
      continue;
    }

    const std::string prefix = key.substr(0, colon);
    const std::string rest = key.substr(colon + 1);
    bool prefixOk = false;
    Category cat = Category::ioctl;
    for (Category c : kAllCategories) {
      if (prefix == categoryPrefix(c)) {
        prefixOk = true;
        cat = c;
      }
    }
    if (!prefixOk) {
      bad.push_back(key);
      continue;
    }
    // A numeric key is always meaningful: it names an id this shim has no name
    // for, which is exactly why the numeric form exists.
    if (rest.size() > 2 && rest[0] == '0' && (rest[1] == 'x' || rest[1] == 'X'))
      continue;
    if (!knownName(cat, rest))
      bad.push_back(key);
  }
  return bad;
}

} // namespace shim
