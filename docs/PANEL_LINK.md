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

What is in the USB-C port (0 nothing, 1 a charger, 2 a computer): on connecting, and
when a program plugs something in or pulls it out.

```
headset 1
```

What is in the headphone jack, as below: on connecting, and at each change.

```
power off
power on
```

Whether the board has its supply: the charger's ship FET, which the firmware works
(`power reboot`: off for 350 ms, then everything starts again; `power ship`, `power
off` and the desktop's Power Off: off until a charger or a computer is plugged in,
`usb 1` or `usb 2` below). While it is off the MCU does not run, and the screen, its
backlight and the LEDs are dark.

```
ambient 25.0
```

The temperature around the device, in °C: on connecting, and when a program sets it.

```
rk3576 on
rk3576 off
```

Whether the RK3576 runs, with a kernel for it (`run --linux`; without one this is never
sent): on connecting, and at each change. It starts when the MCU switches its rail on
(VCC5V0_SYS_S5, the main expander's pin 12) and stops when the rail is cut, or when
Linux powers off (PSCI SYSTEM_OFF: off until the MCU cycles the rail). Linux's reboot
starts it again without an `off` between.

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
usb 2
usb 0
```

Into the USB-C port: a charger, a computer, or nothing. The charger is a USB-C one of
5 V that offers 3 A (its Rp) and shorts D+ to D- (a dedicated charging port; the
firmware's PD does not ask for more); the computer's port gives 5 V and 500 mA (a
standard port), and its data lines. The board's USB mux has them on the MCU while the
Linux side is off: the computer enumerates the MCU's USB device. While it is on (the
MCU firmware switches the mux with its rail) they reach the RK3576's USB0: the computer
enumerates Flipper OS's USB gadget. Either way, the CDC serial port it finds is
`devices/NAME/usb.sock`. The charger chip sees either (and takes 300 ms to know which
it is), the battery charges, the USB-C chips see their partner. What is plugged in
stays when the program goes. The window's Device menu has them (Ctrl+U, Ctrl+Shift+U).

```
headset 1
headset volup
headset 1
headset 0
```

A headset in the 3.5 mm jack (a four-pole one, with a mic), a button on its cable held
down (`play`, `volup`, `voldown`, `assist`: the standard's 0, 240, 470 and 135 Ω across
the mic line) and let go, and the headset pulled out. The firmware looks at the jack
every 100 ms and takes a second after a plug goes in to look for the mic; a button
shorter than that is not seen. The window's Device menu has them (The headphone jack).

```
ambient 40
ambient -10.5
```

The temperature around the device, in °C, from -40 to 125 (to a tenth of a degree):
25 until a program sets it, and it stays when the program goes. The battery takes it at
once: its gauge reads it, and so does the charger's TS input (the charger's die is 5 °C
warmer). The RK3576 warms or cools toward it, with a time constant of 10 s, its own
warmth on top: about 8 °C at rest, and up to 45 °C with every core busy at full clock.
Linux's thermal zones see that:
- from 85 °C on a cluster's sensor, cpufreq's cooling slows the cores, and slower cores
  draw less: at 85 °C around it, the chip holds near 94 °C;
- at 115 °C, Linux shuts down;
- at 120 °C, the TSADC resets the chip, as its CRU does.
From about 105 °C around it, Flipper OS cannot run. The window's Device menu has a few
(The temperature around it).

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
