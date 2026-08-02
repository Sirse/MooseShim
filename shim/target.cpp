#include "target.h"

#include "log.h"

namespace shim {
namespace {

template <typename Fn> bool resolve(HMODULE m, const char *name, Fn *out, std::string *error) {
  FARPROC p = GetProcAddress(m, name);
  if (!p) {
    if (error)
      *error = std::string("missing export ") + name;
    return false;
  }
  *out = reinterpret_cast<Fn>(p);
  return true;
}

} // namespace

bool loadTarget(const std::string &path, Target *t, std::string *error) {
  if (path.empty()) {
    if (error)
      *error = "no target library configured ([target] library=...)";
    return false;
  }

  HMODULE m = LoadLibraryA(path.c_str());
  if (!m) {
    char buf[128];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "LoadLibrary failed, GetLastError=%lu",
                ::GetLastError());
    if (error)
      *error = std::string(buf) + " for " + path;
    return false;
  }

  Target n;
  n.module = m;

  const bool ok =
      resolve(m, "PassThruOpen", &n.Open, error) &&
      resolve(m, "PassThruClose", &n.Close, error) &&
      resolve(m, "PassThruConnect", &n.Connect, error) &&
      resolve(m, "PassThruDisconnect", &n.Disconnect, error) &&
      resolve(m, "PassThruReadMsgs", &n.ReadMsgs, error) &&
      resolve(m, "PassThruWriteMsgs", &n.WriteMsgs, error) &&
      resolve(m, "PassThruStartPeriodicMsg", &n.StartPeriodicMsg, error) &&
      resolve(m, "PassThruStopPeriodicMsg", &n.StopPeriodicMsg, error) &&
      resolve(m, "PassThruStartMsgFilter", &n.StartMsgFilter, error) &&
      resolve(m, "PassThruStopMsgFilter", &n.StopMsgFilter, error) &&
      resolve(m, "PassThruSetProgrammingVoltage", &n.SetProgrammingVoltage, error) &&
      resolve(m, "PassThruReadVersion", &n.ReadVersion, error) &&
      resolve(m, "PassThruGetLastError", &n.GetLastError_, error) &&
      resolve(m, "PassThruIoctl", &n.Ioctl, error);

  if (!ok) {
    FreeLibrary(m);
    return false;
  }

  *t = n;
  SHIM_INFO("Loaded target J2534 driver [%s]", path.c_str());
  return true;
}

} // namespace shim
