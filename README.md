# openrgb-dkms-drivers

## Intro
This repository contains OpenRGB's i2c-piix4 and i2c-nct6775 drivers as DKMS kernel modules.  The i2c-piix4 driver supports the secondary i2c controller on several >= X370 AM4 mainboards. This package is designed for Debian Unstable but might work with earlier versions of Debian, mileage may vary. It is designed as a soft dependency for future packaged versions of [OpenRGB](https://gitlab.com/CalcProgrammer1/OpenRGB) to enable access to RGB controllers on certain mainboards, namely boards with ASUS Aura and ASRock Polychrome RGB systems.

The original idea of patching and packaging this module was *lend* from the package i2c-piix4-aura-dkms found at [Arch Linux AUR](https://aur.archlinux.org/packages/i2c-piix4-aura-dkms). I have only changed some Debian-specific pathes and added some packaging information with debhelper.

## Usage
Clone this repository and run `dpkg-buildpackage -us -uc -b`. Install the resulting package with `sudo dpkg -i path/to.deb`.
To have OpenRGB controlling your LEDs you either need to add your user to the group `adm` or simply run OpenRGB as root. I'd rather not recommend to run any GUI application with root privileges.

## Thanks to 
- [crt0mega](https://github.com/crt0mega/i2c-piix4-aura-dkms) for the original implementation of this package, which did not include i2c-nct6775,
- [CalcProgrammer1](https://gitlab.com/CalcProgrammer1) for his awesome work on OpenAuraSDK/OpenRGB and his fix for i2c-piix4 (I swear that this was already in here when I forked it),
- Térence Clastres for the awesome idea of squeezing this patch into a DKMS package and
- [crt0mega](https://github.com/crt0mega)'s gf for sharing and supporting his obsession with RGB LED stuff and Linux.
- [SurfsideShip264](https://www.reddit.com/user/SurfsideShip264/) for pointing out an issue with the build-dependencies
