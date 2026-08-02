#pragma once

#include "include/J2534_v0404.h"

#include <string>
#include <windows.h>

namespace shim {

// Resolved entry points of the real J2534 driver (Mongoose etc.).
struct Target {
  HMODULE module = nullptr;

  PTOPEN Open = nullptr;
  PTCLOSE Close = nullptr;
  PTCONNECT Connect = nullptr;
  PTDISCONNECT Disconnect = nullptr;
  PTREADMSGS ReadMsgs = nullptr;
  PTWRITEMSGS WriteMsgs = nullptr;
  PTSTARTPERIODICMSG StartPeriodicMsg = nullptr;
  PTSTOPPERIODICMSG StopPeriodicMsg = nullptr;
  PTSTARTMSGFILTER StartMsgFilter = nullptr;
  PTSTOPMSGFILTER StopMsgFilter = nullptr;
  PTSETPROGRAMMINGVOLTAGE SetProgrammingVoltage = nullptr;
  PTREADVERSION ReadVersion = nullptr;
  PTGETLASTERROR GetLastError_ = nullptr;
  PTIOCTL Ioctl = nullptr;

  bool loaded() const { return module != nullptr; }
};

// Loads the target library and resolves every mandatory export. Returns false
// and fills `error` when the library or a mandatory export is missing.
//
// There is deliberately no unload: the target is released only when the process
// exits. Calling FreeLibrary from DllMain(DLL_PROCESS_DETACH) is not allowed -
// the loader lock is held and the target may already be gone - and unloading it
// while SDA still holds channel ids would be worse than leaking the handle.
bool loadTarget(const std::string &path, Target *target, std::string *error);

} // namespace shim
