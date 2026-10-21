# Package Management (`tuxpac`)

Tuxlet includes a custom package manager located at `/usr/bin/tuxpac` that directly parses and installs standard Debian binary archives (`.deb`).

## 1. Configure Mirrors

Repositories are defined in `/etc/tuxlet/mirror` (one entry per line). A ready-made one ships as `/etc/tuxlet/mirror-example`.

**HTTPS Support:** To use `https://` mirrors, first install `curl` via HTTP (it brings `ca-certificates`), then update your mirror file.

## 2. Package Management Commands

| Command | Action |
| --- | --- |
| `tuxpac -y` | Sync local package indexes with configured mirrors |
| `tuxpac -s <pkg>` | Install a package along with its required dependencies |
| `tuxpac -e` | Install Debian essential packages |
| `tuxpac -re` | Remove Debian essential packages |
| `tuxpac -r <pkg>` | Remove a package and unneeded orphaned dependencies |
| `tuxpac -n <pkg>` | Purge a package along with its configuration files |
| `tuxpac -u [pkg]` | Upgrade a specific package or all installed software |
| `tuxpac -f <query>` | Search available repository packages |
| `tuxpac -q [query]` | List installed packages on the system |
| `tuxpac -i <pkg>` | Show detailed package metadata |
