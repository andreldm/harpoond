# harpoond

Provides basic support to control Corsair Harpoon Mouse's LEDs.
Being able to reliably turn off the mouse LED and set its color (static) is what you can expect.

It's not the goal of this project to offer all features present in iCUE, for that OpenRGB and ckb-next are more suited.
Bluetooth support is also out of scope.

## How to build and use

### Linux

You need to have installed a C compiler and libsub, on Arch that's possible with `pacman -S gcc libusb`, while on Ubuntu/Debian `apt install gcc libusb-1.0-0-dev`.

```
git clone https://github.com/andreldm/harpoond
cd harpoond
# Tweak values in harpoond.c, where it says "Set custom configuration"
make
sudo make install
systemctl --user enable --now harpoond.service
```

### Windows

While it's possible to build this project with Visual Studio, I recommend [MSYS2](https://www.msys2.org/) because it's easier to get libusb to work and requires no code changes.

In a mingw64 shell, run:

```
pacman -S git make pkg-config mingw-w64-x86_64-gcc mingw-w64-x86_64-libusb
git clone https://github.com/andreldm/harpoond
cd harpoond
# Tweak values in harpoond.c, where it says "Set custom configuration"
make -f Makefile.msys2
# Press Win + R, execute shell:startup, create a shortcut to harpoon.exe in that folder
```
