# MooseShim

[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/Sirse/MooseShim/)

MooseShim is a 32-bit J2534 compatibility layer that lets some vendor specific
software (e.g. SDA) use a generic J2534 adapter in place of DiCE. It has been
developed and tested with MongoosePro JLR.

The shim forwards standard J2534 calls, translates the DiCE-specific flags SDA
uses, routes MS-CAN through `ISO15765_PS` when required, and absorbs vendor IDs
that would have a different meaning in the target driver.

MooseShim does not modify SDA or the target J2534 driver.

## Scope and limitations

- Supported: ISO15765/UDS communication used by P3, SPA and related platforms.
- Experimental: Volvo D2/DII translation over standard raw `CAN`/`CAN_PS`.
  It is disabled by default (`[channel] d2_route=reject`).
- Unsupported: payloads in the proprietary DiCE
  `ISO15765_SWDL_FABRICATE` mode that exceed one ISO-TP message. Writes above
  4095 payload bytes (plus the four-byte CAN identifier) fail with
  `ERR_NOT_SUPPORTED` instead of being forwarded malformed. SDA leaves this
  channel option enabled during ordinary UDS traffic, so normally sized writes
  continue to use the standard ISO15765 path.

### Experimental D2/DII

Enable only after a mock or bench qualification by selecting `d2_route=can`,
`can_ps` or `auto`. The current transport covers 125, 250 and 500 kbit/s,
29-bit CAN and SDA-provided sample points. `auto` retries `CAN_PS` only after a
capability error; device and channel errors do not trigger fallback.

The translation is implemented in the shim and exposes only standard target
protocols. Multi-frame periodic messages use a shim-owned scheduler. Relay-
dependent configurations are unsupported, D2 protocol acknowledgements and
flashing are not qualified, and a live test on the intended adapter and vehicle
is required before using it for programming.

## Configuration

`MooseShim.ini` must be located beside the DLL. Set `[target] library` to the
real 32-bit J2534 DLL. The supplied configuration targets MongoosePro JLR.
`adapter_profile` controls compatibility behaviour:

- `generic` keeps strict J2534 v04.04 semantics;
- `auto` enables only workarounds gated by a concrete target refusal;
- `mongoose` additionally tolerates the observed unacknowledged functional
  broadcast timeout.

The individual `[channel]` switches remain supported and override the profile.
Other adapters are candidates rather than qualified devices until the SDA
integration suite has been run against their driver.

Important compatibility options are documented directly in the INI:

- MS-CAN pin-switched fallback;
- `CLEAR_RX_BUFFER` emulation;
- normalization of empty blocking reads;
- handling of unacknowledged functional broadcasts;
- unambiguous correction of SDA filter cleanup on the wrong bus channel;
- logging level and path.

Unknown IDs in the vendor range are absorbed by default. Do not disable this
protection casually: DiCE and DrewTech assign different meanings to the same
vendor numbers.

## Registration

Copy `tools/register.reg.template`, replace `@MOOSESHIM_DLL@` with the absolute
escaped path to `MooseShim.dll`, and import the resulting `.reg` file as an
administrator. The key must remain under `WOW6432Node` because SDA is 32-bit.

Select `MooseShim` once in SDA under `Options -> Communication settings`.

## Logging

The default log is `%TEMP%\mooseshim.log`. Normal operation uses `debug`; use
`trace` only for short captures because it records every J2534 call. Logs rotate
at 10 MiB and retain five `.1`-`.5` generations. Message payloads are capped at
64 bytes in the log while the original `DataSize` remains visible.

## Development

## Build

SDA is a 32-bit process, so MooseShim must be built for Win32:

```powershell
cmake -S . -B build-x86 -G "Visual Studio 17 2022" -A Win32
cmake --build build-x86 --config Release
```

The runnable files are produced in `build-x86/Release`:

- `MooseShim.dll`
- `MooseShim.ini`

The DLL uses the static MSVC runtime and exports undecorated J2534 entry points.

## Tests

The test suite never opens real hardware. Integration tests load MooseShim over
the bundled mock J2534 driver and cover the compatibility paths observed with
SDA, including MS-CAN fallback, channel cleanup, periodic-message starvation,
functional addressing and vendor-ID filtering.

```powershell
cmake --build build-x86 --config Release
ctest --test-dir build-x86 -C Release --output-on-failure
```

`mock/`, `tests/` and the vendored doctest header are part of the maintained
regression suite rather than runtime dependencies. The mock runs in Mongoose,
strict-J2534 and native-dual-CAN modes so compatibility workarounds cannot
silently become requirements for otherwise compliant adapters.

## Support

If MooseShim is useful to you, donations are welcome:

| Asset | Network | Address |
| --- | --- | --- |
| USDT | TON | `UQCxMCdWqI4WARWedMd4h59iOHougyxbxl70IJdH2VlAC-G0` |
| USDT | TRON | `TMgAEoeRHuh1nFpqsJoav7KZLaXeq5Wgr2` |
| BTC | Bitcoin | `bc1qv7ggns2g5wvgk76x2a0062pya2armvqqhvtcm6` |
