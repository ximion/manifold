# Manifold

**Image-based deployment for system-integrated Linux applications.**

Manifold is a deployment format and toolset for Linux applications and plugins
that need the portability of a container (one consistent library environment on
every distribution) as well as the system integration of a native package:
udev rules, systemd services, device access, and (mostly) unrestricted interaction
with the host.

Manifold fills the gap between AppImage (no system integration), Flatpak (too
restrictive for some software) and distribution packages (not portable across
distributions). It is inspired by Steam's Pressure Vessel, and uses a lot of the
same technology that Valve uses for Steam as well.

Manifold's primary audience is vendors of industrial, scientific, and
professional Linux software, such as lab instruments, CNC controllers, image analysis
tools, development environments, and similar applications that don't fit the
consumer desktop app model that Flatpak already serves well.

> [!WARNING]
> Manifold is experimental. Its formats, interfaces, and APIs may still change in
> incompatible ways. A predecessor of this project has been deployed successfully
> in production, but the public version differs significantly from the internal one
> and has so far received much less testing.

## Design principles

**Deployment is not sandboxing.** Manifold splits the reproducible/compatible environment
problem (consistent libraries across distributions) from the isolation problem
(restricting what an application can do). Only the first problem is solved by Manifold.
By default, nothing sits between an application and the system: There is no sandbox,
no seccomp filter, and no supervising process. The application is executed in place.
Isolation, if an application wants it, is meant to be added by other tools.
Manifold will provide sandboxing hooks, but apart from that expects the application to do
any sandboxing on its own

**Built on stable distributions.** Curating a parallel package ecosystem and tracking dependencies
takes an enormous amount of ongoing work, and Debian, Ubuntu LTS, AlmaLinux, and similar projects
already do it. Manifold uses them as its runtime substrate instead of duplicating that effort.
This results in familiar build environments and blazingly fast builds of application images.
Runtime images consist only of distribution packages, and can be reproduced from their package
list. Application vendors target a known stable distribution, and Manifold delivers the
result to whatever distribution the host runs.

**Reuse the ecosystem.** Manifold uses existing, well-maintained components.
It contains no new filesystem, image format, or sandbox. It mostly just combines
well-established existing technology:

- Disk images laid out like [UAPI Discoverable Disk Images](https://uapi-group.org/specifications/specs/discoverable_disk_image/),
  with an EROFS filesystem partition, a dm-verity hash partition, and a signature
  partition.
- libcryptsetup to set up dm-verity on loop devices.
- overlayfs and the kernel's new mount API to assemble mount trees and hand them over to the launcher.
- `capsule-capture-libs` from [libcapsule](https://gitlab.com/fledermaus/libcapsule) to
  pass the host's graphics stack through.
- systemd for Varlink and various useful utilities.
- AppStream MetaInfo for component metadata.

**Integrate with the host.** Manifold applications are first-class citizens on the
host system. They run in a mount namespace that exists for library consistency, not
for isolation: the user's home, `/dev`, D-Bus, the user's runtime directory, and the
host's own filesystem are all visible inside it. A Manifold application launched from
a systemd unit *is* that unit's main process. Manifold applications may install aliases
into `PATH`, install udev rules, systemd units, MIME types, etc. in a declarative way.

**Take the host's drivers.** The graphics and compute drivers come from the
host, because that is the driver stack the host system is certified for.
Everything else comes from the images, with a few exceptions where the respective
newer copy wins.


## Non-goals

- **Replacing Flatpak for consumer desktop apps.** Flatpak's sandbox model works for
  that use case. Manifold targets software for which that model doesn't fit.
- **Portability across architectures.** Images are architecture-specific; x86_64 and
  aarch64 builds are separate artifacts. Installing an image built for another
  architecture is refused (for now at least).
- **A standalone, unprivileged mode.** Manifold requires a privileged installation step,
  and applications are installed system-wide. The AppImage-like use case is out of scope.
- **A sandbox.** When a user launches an application, it runs in a user namespace, but
  only because that is how a process without privileges gets a mount namespace of its
  own. Nothing is hidden from the application.
- **Support for non-Glibc hosts.** Manifold does not support hosts that do not use Glibc
  currently. It targets Linux distributions with a GNU or GNU-compatible userland only.


## Building

### Dependencies

- Meson ≥ 0.62 and a C compiler that supports C23
- GLib, GObject, and GIO ≥ 2.80
- libsystemd ≥ 257, plus the systemd and udev pkg-config files
- libcryptsetup ≥ 2.4
- libzip ≥ 1.7
- libblake3
- AppStream
- libcurl ≥ 7.62
- libelf ≥ 0.152, for `capsule-capture-libs`
- libglnx, which Meson fetches as a subproject

Optional:

- gobject-introspection, to build the GObject Introspection data
- `apparmor_parser` ≥ 4.0, to install the AppArmor profile
- Sphinx, to build the documentation (on by default; disable it with `-Ddocs=false`)

At runtime, Manifold needs systemd, polkit, and a kernel with loop devices,
device-mapper verity, EROFS, and overlayfs.

### Building and installing

```sh
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build

sudo systemctl daemon-reload
sudo systemctl enable --now manifold.socket manifold-run.socket
```

## Contributing

Patches, pull-requests, and bug reports are very welcome! Please make sure your code
is valid C23 and formats cleanly with `clang-format` (you can use the `./autoformat.py`
script for formatting).

## License

Manifold is licensed under the [GNU Lesser General Public License v2.1 or later](LICENSE).

