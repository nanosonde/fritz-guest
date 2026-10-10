# fritz-guest

Extend a FRITZ!Box guest network to an OpenWrt router.

`fritz-guest` is a small native C daemon for OpenWrt that opens an
L2TPv3 Ethernet pseudowire (RFC 3931 / RFC 4719, IP protocol 115) to the
FRITZ!Box guest service `fritz.box:guest` and attaches the resulting
interface `fritzguest0` to an existing guest bridge. Guest SSIDs or LAN ports
on that bridge then become part of the FRITZ!Box guest network: the
FRITZ!Box keeps doing DHCP, NAT and the guest firewall.

```mermaid
flowchart LR
    W["Guest SSID or guest LAN port"] --- B["br-guest (OpenWrt)"]
    B --- E["fritzguest0"]
    E <-->|"IP protocol 115 over the home LAN"| F["FRITZ!Box guest network"]
    F --> I["Internet"]
```

> **Status:** preview. Passes the automated host and kernel-namespace tests
> and works on a real router (OpenWrt 25.12.5, mediatek/filogic): guest
> clients behind `fritzguest0` get their address from the FRITZ!Box and
> reach the Internet. The remaining real-device checks ([SPEC.md](SPEC.md)
> section 13: recovery, isolation, upgrades) are still open.

It is not AVM Mesh, does not copy Wi-Fi settings and is **not a VPN**: guest
frames cross the home LAN unencrypted.

## Packages

| Package | Contents |
|---|---|
| `fritz-guest` | `/usr/sbin/fritz-guestd`, `/etc/init.d/fritz-guest`, `/etc/config/fritz-guest`; depends on libubox, libubus, libuci, libmnl, kmod-l2tp-ip, kmod-l2tp-eth |
| `luci-app-fritz-guest` | Optional page *Services → FRITZ! Guest Network* for configuration and live status |

## Setup

1. Create a dedicated guest bridge and a `proto none` interface that uses it
   (Network → Interfaces, or `/etc/config/network`):

   ```uci
   config device
       option name 'br-guest'
       option type 'bridge'
       option bridge_empty '1'

   config interface 'guest_extension'
       option device 'br-guest'
       option proto 'none'
   ```

   Attach a guest SSID to `guest_extension` or add dedicated LAN ports to
   `br-guest`. Never put the home LAN uplink into this bridge.

2. Install the packages and configure `/etc/config/fritz-guest` (or use LuCI):

   ```uci
   config connection 'guest'
       option enabled '1'
       option server '192.168.178.1'
       option uplink 'lan'
       option bridge 'br-guest'
       option mtu '1500'
       option hello_interval '30'
       option retry_max '5'
       option reconnect_max '60'
   ```

   `uplink` is a logical interface on the home LAN, `bridge` is a bridge
   *device*. The package is disabled by default and never changes network,
   wireless, DHCP or firewall configuration.

3. Apply with LuCI *Save & Apply*, or `uci commit fritz-guest && reload_config`.

## Operation

* Status: `ubus call fritz-guest status` (state, reason, negotiated IDs,
  counters). States: `disabled`, `waiting`, `connecting`, `connected`,
  `backoff`, `stopping`, `error`.
* Logs: `logread -e fritz-guestd`. Add `--debug` to the procd command for
  per-message traces (headers only, never payloads).
* Validate without side effects: `fritz-guestd --check-config`.
* Reloads are validated first; a rejected configuration keeps the running one
  and is reported as `reload_error`.
* Stop: `/etc/init.d/fritz-guest stop` closes the connection (up to 31 s with
  `retry_max 5`) and removes `fritzguest0`. If the daemon dies, the kernel
  removes the tunnel and device together with the daemon's socket.
* An existing `fritzguest0` that the daemon did not create is never adopted
  or deleted.

### Firewall and isolation

* IP protocol 115 between OpenWrt and the FRITZ!Box on the uplink must pass.
* Guest traffic is bridged, not routed. Isolating guests from the home LAN and
  from the router's own services (including IPv6 link-local) needs your own
  firewall policy. Do not run DHCP/RA on the guest interface and do not give
  it a management address unless you mean to expose the router to guests.
* Separate tunnels do **not** isolate guests from each other; guest-to-guest
  traffic follows the FRITZ!Box configuration.

## Building

Requirements: [just](https://just.systems), `curl` or `wget`, `zstd`, a C
compiler and the usual OpenWrt SDK host tools. Everything stays inside this
directory: the SDK goes to `sdks/`, packages to `dist/<target>/`, test
binaries to `build/`.

```sh
just package-build                   # downloads the OpenWrt 25.12.5 SDK once, builds both .apk
just package-build target=x86/64     # other targets
just test                            # protocol/FSM/UCI host test (ASan/UBSan) + LuCI contract (Node.js)
just test-kernel                     # after package-build: data plane in an unprivileged netns
just check                           # init script syntax and JSON files
```

The first build compiles libubox, ubus, uci and libmnl inside the SDK; later
builds only rebuild the two packages. Install on the router with
`apk add --allow-untrusted fritz-guest-*.apk luci-app-fritz-guest-*.apk`.

## Repository layout

```text
src/        daemon: proto.c (codec), ctrl.c (reliable transport + FSM),
            config.c/uciconf.c (UCI), linux.c (L2TP/rtnetlink via libmnl),
            netifd.c (bridge membership via ubus), main.c (uloop, ubus status)
tests/      protocol-test.c, kernel-test.c, luci-contract.js
package/    OpenWrt package recipes
docs/       protocol notes and interoperability tests
tools/      fritz_guest_probe.py, a diagnostic userspace endpoint (not installed)
SPEC.md     design specification
```

## Known limitations

* One fixed IPv4 server, one session, one bridge; the server must be on the
  uplink's local network. No server discovery, no outer IPv6.
* Fatal protocol errors close the whole connection with StopCCN instead of a
  CDN first (RFC-conformant with a single session).
* HELLO keepalives are driven by control-channel silence only.
* Full-size guest frames produce 1542-byte outer packets that are IPv4
  fragmented on a standard 1500-byte LAN.

## Further reading

* [SPEC.md](SPEC.md): behaviour, UCI and ubus contracts, test plan
* [docs/protocol.md](docs/protocol.md): wire protocol and interoperability tests

## License

GPL-2.0-or-later, see [LICENSE](LICENSE).

## Disclaimer

This is an independent project, not affiliated with or endorsed by AVM.
FRITZ! and FRITZ!Box are trademarks of AVM GmbH. Use it only on networks you
administer.
