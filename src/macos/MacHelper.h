#pragma once

#include <QString>

// The privileged helper on macOS, see macos/ocg-helper-protocol.h.
namespace MacHelper {

// Version reported by the installed helper, or -1 when it does not answer.
int installedVersion();

// True when the installed helper matches this build.
bool ready();

// Offers to install (or update) the helper if needed. Returns true when the
// application can run without administrator rights.
bool ensure();

// Command for openconnect_setup_tun_script().
QByteArray tunScriptCommand(const QString& interfaceName);

// libopenconnect keeps the default buffer of its end of the tun script
// socket pair: a few KB on macOS, two or three packets. Enlarge it.
void enlargeTunSocketBuffers();

}
