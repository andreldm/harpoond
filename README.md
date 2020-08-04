# harpoond

Provides basic support to control Corsair Harpoon Mouse's LEDs.
Being able to reliably turn off the mouse LED and set its color (static) is what you can expect.

It's not the goal of this project to offer all features present in iCUE, for that OpenRGB and ckb-next are more suited.
Bluetooth support is also out of scope.

## How to use

```
git clone https://github.com/andreldm/harpoond
cd harpoond
# Tweak values in harpoond.c, where it says "Set custom configuration"
make
sudo make install
systemctl --user enable --now harpoond.service
```
