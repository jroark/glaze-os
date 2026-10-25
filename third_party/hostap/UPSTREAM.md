# wpa_supplicant PSK core

Source: https://w1.fi/releases/wpa_supplicant-2.11.tar.gz
Version: 2.11
Archive SHA-256: 912ea06f74e30a8e36fbb68064d6cdff218d8d591db0fc5d75dee6c81ac7fc0a
License: BSD-3-Clause (COPYING and README).

Only the WPA/RSN supplicant, PSK/IE helpers, internal crypto primitives, utility
sources, and headers required by the enabled profile are retained. No EAP,
OS daemon, driver implementation, control socket, or configuration parser is
built. Profile: servers/net/wpa/config.h. OS callbacks, bounded timers, frame
transport, random input, firmware key callbacks, and authorization are in
servers/net/wifi_supplicant.c.

Local upstream changes are limited to GLAZE_HOSTAP guards in utils/includes.h,
utils/os.h, and utils/common.h. They avoid unavailable POSIX/glibc headers and
provide the target's little-endian byte-swap definitions. Two existing
space-before-tab indentation sequences in common.h are normalized. State-machine and
cryptographic sources are unmodified. Upstream debug logging is disabled,
including key dumps. The existing upstream no-802.1X PMKSA stubs are used.
