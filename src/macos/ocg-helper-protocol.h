/*
 * Protocol between OpenConnect-GUI and its privileged helper on macOS.
 *
 * The GUI runs as the logged-in user. Only two things need root: creating
 * the utun interface and running vpnc-script. The helper (a launchd daemon)
 * does both on behalf of ocg-tun-client, which libopenconnect starts as its
 * tun script (openconnect_setup_tun_script()).
 *
 * Over a stream socket the client sends one request; for OCG_OP_CONNECT the
 * payload is the vpnc-script environment as NUL-terminated "NAME=value"
 * strings. The reply carries the status, the interface name and the script
 * output, plus the utun descriptor (SCM_RIGHTS). The connection then stays
 * open; when the client closes it the helper runs vpnc-script with
 * reason=disconnect.
 */
#pragma once

#include <stdint.h>

#define OCG_HELPER_LABEL "net.openconnect-vpn.gui.helper"
#define OCG_HELPER_SOCKET "/var/run/" OCG_HELPER_LABEL ".sock"
#define OCG_HELPER_BIN "/Library/PrivilegedHelperTools/" OCG_HELPER_LABEL
#define OCG_HELPER_SCRIPT "/Library/PrivilegedHelperTools/net.openconnect-vpn.gui.vpnc-script"
#define OCG_HELPER_PLIST "/Library/LaunchDaemons/" OCG_HELPER_LABEL ".plist"

/* Bump on any change of the helper or of the protocol: the GUI offers to
 * reinstall a helper that reports another version. */
#define OCG_HELPER_VERSION 1

#define OCG_MAGIC 0x4f434748u /* "OCGH" */

enum {
    OCG_OP_VERSION = 1,
    OCG_OP_CONNECT = 2,
};

/* Request: { magic, version, op, len }; reply: { magic, version, status, len }.
 * status is 0 or an errno value. Integers in host byte order. */
struct ocg_msg_hdr {
    uint32_t magic;
    uint32_t version;
    uint32_t code;
    uint32_t len;
};

#define OCG_MAX_PAYLOAD (256 * 1024)

/* Written by ocg-helper --install to stdout, read by the GUI. */
#define OCG_INSTALL_OK "OCG-INSTALL-OK"
#define OCG_INSTALL_ERR "OCG-INSTALL-ERR"
#define OCG_INSTALL_SETTINGS "OCG-SETTINGS"
