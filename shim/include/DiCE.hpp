#pragma once

#include "J2534_v0404.h"

namespace j2534 {
namespace dice {

// Device-level IOCTLs from TSDiCE32.h. The test values are intentionally
// listed explicitly to make the wire-level ABI visible.
constexpr unsigned long TEST_CARB_PLUG_CONNECTED = 0x10000001;
constexpr unsigned long TEST_EEPROM = 0x10000002;
constexpr unsigned long TEST_FLASH_INTERNAL = 0x10000003;
constexpr unsigned long TEST_FLASH_EXTERNAL = 0x10000004;
constexpr unsigned long TEST_RAM_INTERNAL = 0x10000005;
constexpr unsigned long TEST_RAM_EXTERNAL = 0x10000006;
constexpr unsigned long TEST_LED = 0x10000007;
constexpr unsigned long TEST_PC_DEVICE_LOOPBACK = 0x10000008;
constexpr unsigned long TEST_K_PIN7_TO_L_LOOPBACK = 0x10000009;
constexpr unsigned long TEST_K_PIN11_TO_L_LOOPBACK = 0x1000000A;
constexpr unsigned long TEST_L_TO_K_PIN7_LOOPBACK = 0x1000000B;
constexpr unsigned long TEST_L_TO_K_PIN11_LOOPBACK = 0x1000000C;
constexpr unsigned long TEST_CAN_MS_CAN_HS_LOOPBACK = 0x1000000D;
constexpr unsigned long TEST_CAN_HS_CAN_MS_LOOPBACK = 0x1000000E;
constexpr unsigned long WARRANTY_CLOCK_GET = 0x10001001;
constexpr unsigned long GET_HW_VERSION = 0x10001002;
constexpr unsigned long ENABLE_BT = 0x10001003;

constexpr unsigned long CAN_XON_XOFF_STMIN = 0x10000001;
constexpr unsigned long ISO15765_SWDL_FABRICATE = 0x10000002;

// DiCE protocol identifier for mixed RawCAN and ISO15765 frames.
constexpr unsigned long MIXED_FORMAT = 0x10000002;

constexpr unsigned long FUNC_ADDR = (1UL << 28);
constexpr unsigned long CAN_FRAME_RAW = (1UL << 10);
constexpr unsigned long TX_INDICATION = (1UL << 3);

// FUNC_ADDR (0x10000000) collides with DrewTech SNIFF_MODE; use only with the
// DiCE adapter and protocol context that defines FUNC_ADDR.

// DICE_CAN_XON_XOFF_FILTER (0x20000001) is also DrewTech DT_READ_DIO. Its
// meaning depends on the adapter, so do not use it across vendor
// implementations.
constexpr unsigned long DICE_CAN_XON_XOFF_FILTER = 0x20000001;

enum LED_NO : unsigned long {
  RED = 0,
  GREEN = 1,
  BLUE = 2,
  YELLOW = 3,
  ORANGE = 4,
  RELEASE = 5,
};

enum LED_STATE : unsigned long {
  OFF = 0,
  ON = 1,
  BLINK = 2,
};

struct S_LED_CONTROL {
  LED_NO ledNo;
  LED_STATE ledState;
  unsigned long ledBlinkPeriod;
};

struct S_XON_XOFF_FILTER {
  PASSTHRU_MSG rXOnMaskMsg;
  PASSTHRU_MSG rXOnPatternMsg;
  PASSTHRU_MSG rXOffMaskMsg;
  PASSTHRU_MSG rXOffPatternMsg;
};

} // namespace dice
} // namespace j2534
