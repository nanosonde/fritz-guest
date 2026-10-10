# fritz-guest backend package

Builds the repository's `src/` daemon with the OpenWrt SDK (`just package-build`)
and installs:

| Source | Installed path |
|---|---|
| `src/` (fritz-guestd) | /usr/sbin/fritz-guestd |
| files/etc/config/fritz-guest | /etc/config/fritz-guest (conffile) |
| files/etc/init.d/fritz-guest | /etc/init.d/fritz-guest |

Runtime dependencies: libubox, libubus, libuci, libmnl, kmod-l2tp-ip,
kmod-l2tp-eth (pulls kmod-l2tp). No Python, iputils or ip-full.

The init script always runs the daemon under procd, also when `enabled` is
`0` (it then holds no tunnel resources). UCI commits of `fritz-guest` or
`network` and uplink interface events trigger `reload`; procd sends SIGHUP
to the unchanged instance and the daemon validates before applying. This is
what makes LuCI Save & Apply start/stop connectivity. `term_timeout` is
derived from `retry_max` (40 s for the default) so StopCCN can be delivered.
