"""Checks that need hands. They are reported as MANUAL with their steps, and
can be run later with `--case manual/<name>` once someone is at the device:
the case then only records what to look for in the console."""
from ..runner import case

M = 'manual'


@case(M, 'usb-unplug', covers='USB', manual=(
    'With a Wi-Fi debug session open (--host), pull the USB cable for 10 s and plug it back in; '
    'the device must keep running, the network session survive, and `glaze-usb probe` over USB answer again.'))
def usb_unplug(ctx):
    pass


@case(M, 'battery-pull-cold-boot', covers='NAND boot', manual=(
    'Battery out 30 s with no card, then Power: Glaze must boot from NAND (gates at true power-on). '
    'Check the boot banner and `springboard: ready` in the log.'))
def cold_boot(ctx):
    pass


@case(M, 'card-hot-swap', covers='#111 #114', manual=(
    'Remove and reinsert the WL54-CF while unlocked, then while asleep: `cf: network card` must follow '
    'each insertion on the first try (open bug: a first insertion after a NAND cold boot goes unseen).'))
def card_swap(ctx):
    pass


@case(M, 'hibernate-power-wake', covers='POWER', manual=(
    'On the NAND build, Settings → Power off → Hibernate, wait 60 s, press Power: the SPL must resume '
    '(`verify: 0 pages changed` in a HIBERNATE_VERIFY build).'))
def hibernate_power(ctx):
    pass


@case(M, 'stowaway', covers='#109', manual=(
    'Dock the Stowaway: type in Notes, check Fn combinations (arrows, Home/End) and key repeat.'))
def stowaway(ctx):
    pass


@case(M, 'gps-outdoor', covers='#110', manual='Outdoors with the GPS-CF: Maps must show a fix and follow movement.')
def gps(ctx):
    pass


@case(M, 'modem-call', covers='#105', manual=(
    'Modem on a phone line: dial, PPP up, load a page. Known-good rig: USR XJ/CC1560 in the '
    'jacket, Teltone TLS-3B line 1, DCB IP-6690 on line 2 (dial 102, PAP; docs/hardware/MODEM.md).'))
def modem(ctx):
    pass


@case(M, 'installer-windows', covers='installer #3 #4', manual=(
    'Glaze Installer 0.2.0 on a real Windows PC: probe, back up, install, update.'))
def installer_windows(ctx):
    pass
