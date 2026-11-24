# The front-panel link

The window that looks like a Flipper One (`scripts/fo_gui.py`, `./flipperoneemu gui`) is
a program apart from QEMU. It gets everything it shows, and sends every button press and
touch, over one link: the Flipper One's front panel. Any other program can be that
window too: a test that presses buttons, a recorder, `./flipperoneemu shot`.

It is FlipperZeroEmu's panel link in its form (lines of ASCII on a Unix socket, one
program at a time), with the Flipper One's screen, lights and buttons.

## Where

Each device has a Unix socket, `devices/NAME/panel.sock`. QEMU listens on it and takes
one client at a time. The launcher passes `-chardev socket,id=panel,path=…,server=on,
wait=off -global flipper-one-panel.link=panel`; running QEMU by hand, give the panel any
chardev as `link`.

## Lines

A message is one line of ASCII text ended by `\n`, its fields separated by spaces.
Unknown messages are ignored, so either side can add to them. A `hello <text>` line from
the program describes it, and QEMU logs it.

### From the emulator

`hello flipperoneemu panel 1` comes first (the `1` is the protocol's version), then
everything below once, so that the program can draw the panel as it is; after that,
each message when what it reports changes. The panel looks every 16 ms of the host's
time.

```
running 1
running 0 paused
```

Whether time goes on in the emulator; when it does not, why: `paused`, `debug` (a
debugger has it stopped), or QEMU's name for its run state.

```
lcd on
lcd off
```

Whether the display controller's display is on. While it is off the panel shows no
picture, whatever the controller's memory holds.

```
rows 32 16 AAAA…
```

Rows of the picture: the first row, how many, and their pixels in base64, 256 bytes a
row, 0 black to 255 white, as the LCD's controller has them (the panel's 64 grey levels
are its top six bits). The screen is 256 × 144. On connecting the whole picture comes;
after that the rows that changed, in runs of 16 rows at most.

```
backlight 255
```

The backlight, 0 to 255.

```
leds 000000 000000 ff0000 …
```

The 17 LEDs as `rrggbb`, in the order the WS2812s are chained: Net (LINK), Wi-Fi, Eth2
(ETH 1), Eth1 (ETH 0); Power (the Power key), the battery's outline (two), the battery's
watts 1 to 4; USB charging, USB watts 1 to 4, the battery's centre.

```
haptic 1
haptic 0
```

Whether the vibration motor runs (the DRV2605L drives it). A buzz shorter than the
panel's 16 ms still comes, as `haptic 1` for one look and `haptic 0` at the next. The
window shakes its drawing while it runs.

```
usb 1
```

Whether a charger is in the USB-C port: on connecting, and when a program plugs one in
or pulls it out.

### To the emulator

```
hello <text>
press ok
release ok
```

A button goes down, and up: `esc`, `view`, `power`, `edit`, `run` (the soft keys under
the screen, left to right), `up`, `down`, `left`, `right`, `ok` (the pad), `back`,
`switch` (App Switcher), `ptt`. A button stays down until it is released. When the
program goes, the buttons it held are released.

```
touch 500 500 128
touch end
```

A finger on the touchpad, at x and y from 0 to 1000 across it (0, 0 the top left), with
a pressure from 0 to 255; and the finger lifted.

```
usb 1
usb 0
```

A charger plugged into the USB-C port, and pulled out: a USB-C charger of 5 V that
offers 3 A (its Rp) and shorts D+ to D- (a dedicated charging port; the firmware's PD
does not ask for more). The charger chip sees it (and takes 300 ms to know what it is),
the battery charges, the USB-C chips see their partner. It stays when the program goes.
The window's Device menu has it (Ctrl+U).

```
refresh
```

Everything again, as on connecting.

## Trying it

```sh
socat - UNIX-CONNECT:devices/NAME/panel.sock
hello me
press ok
release ok
```
