// Notifications des autres applications : pas encore prises en charge sous Linux.
// Piste : écouter org.freedesktop.Notifications.Notify sur le bus de session (BecomeMonitor).
#include "features/appnotifs.h"

#include "web/bridge.h"

namespace appnotifs {

void SetEnabled(bool on) {
    if (on) bridge::PostToPage("{\"t\":\"appnotifs\",\"status\":\"unsupported\"}");
}

void OpenApp(const std::string&) {}

} // namespace appnotifs
