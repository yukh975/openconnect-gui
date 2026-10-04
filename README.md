# OpenConnect GUI

This is the development space of OpenConnect VPN graphical client (GUI).
See the [OpenConnect VPN GUI web site](https://gui.openconnect-vpn.net/)
for detailed description, screen shots and other related projects.


## macOS (Apple Silicon) build of this fork

This fork adds a native arm64 build for macOS: no Rosetta, and no
administrator password at every start. A small privileged helper creates
the VPN interface and runs vpnc-script; the application itself runs as
the user. Upstream: https://gitlab.com/openconnect/openconnect-gui

**Download:** [latest release](https://github.com/yukh975/openconnect-gui/releases/latest),
file `OpenConnect-GUI-<version>-macos-arm64.zip`. Requires macOS 27 on
Apple Silicon.

**Install:**

1. Unpack the archive and move `OpenConnect-GUI.app` to `/Applications`.
2. The build is not signed with an Apple Developer certificate, so macOS
   may refuse to open it after a download from the browser. Then run once:

   ```bash
   xattr -dr com.apple.quarantine /Applications/OpenConnect-GUI.app
   ```

3. On the first start choose **Install** to set up the helper: this asks
   for the administrator password once. Profiles of OpenConnect-GUI 1.5.x
   are imported.

**Remove the helper:**

```bash
sudo /Library/PrivilegedHelperTools/net.openconnect-vpn.gui.helper --uninstall
```

**Build from source:**

```bash
brew install cmake pkgconf qtbase qtscxml spdlog openconnect
contrib/build_macos.sh
```

The self-contained application is put into `build/dist/OpenConnect-GUI.app`.


## Goals of this client

The goal is to have a simple / minimalistic interface to access
enterprise VPN services. Non technical audience is the focus; anyone
should be able to use it.

For contributions we follow:
https://developer.apple.com/design/human-interface-guidelines
where it applies.

### Main tasks

These tasks are a click away:

 - Connecting to a new server
 - Connecting to an existing server
 - Disconnecting
 - View log

### Security

As non-technical audience is the focus of this client it is imperative
that security decisions are not delegated to the user unless absolutely
necessary.

#### Server certificate validation

Historically the SSL VPN servers openconnect works with, had certificates with
incorrect hostnames in them, and were not in the Internet PKI. For that the
way openconnect gui works is
 1. Try Internet PKI validation - if successful server is validated
 2. Fallback to SSH-type authentication where the server public key must remain
    unchanged.


## Supported Platforms
- Microsoft Windows 10 and newer
- macOS 10.12 and newer

## Development info
- [Compilation](docs/dev.md)
- [Development with QtCreator](docs/dev_QtCreator.md)

## Other
- [Creating release package](docs/release.md)
- [OpenConnect library compilation and dependencies](docs/openconnect.md)
- [Web page maintenance](https://gitlab.com/openconnect/openconnect-gui-web)
- [Snapshot builds](docs/snapshots.md)
- [AppVeyor CI builds](https://ci.appveyor.com/project/nmav/openconnect-gui/history)

# License
The content of this project itself is licensed under the [GNU General Public License v2](LICENSE.txt)
