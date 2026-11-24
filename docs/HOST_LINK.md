# The host link

The Flipper One's two chips talk over a link: the RK3576 (Flipper OS) is the I2C master
of the MCU's I2C1, which the MCU firmware serves in slave mode at 0x69, and the MCU pulls
CPU_INT (GPIO35, open drain) low when it has something for the RK3576 to read. Its screen
comes over SPI (GPIO25-28), not here.

Until the RK3576 is emulated, a program can take its place on the I2C side: the host
link is a socket, `devices/NAME/host.sock`, with a line protocol like the front panel's
([PANEL_LINK.md](PANEL_LINK.md)): ASCII lines, one message each, `\n`-terminated. One
program at a time. Running QEMU by hand, give it a chardev:
`-chardev socket,id=host,path=…,server=on,wait=off -global flipper-one-host.link=host`.

The MCU's side of the protocol is its `i2c_intercom` service: 16-bit registers, a write
starting with the register's address (high byte first) and going on to the next
registers, values low byte first; a read serving bytes from the address last written.
Their map is the firmware's `applications/services/i2c_intercom/i2c_registers_map.h`.

## To the emulator

```
write 0040 0003
```

Registers from the address on: one I2C write to 0x69 of the address (high byte first)
and each value (low byte first), then a STOP. The reply is `ok`.

```
read 0080 1
```

Registers from the address: the address written without a STOP, a repeated START, two
bytes a register read (the last NACKed), a STOP. The reply is `regs 0080 0001`: the
address and the values.

```
send 00 40 03 00
recv 4
```

The same, byte by byte: a write of these bytes, or a read of so many. The replies are
`ok` and `got 01 00 00 00`.

Each transfer goes as the RK3576's controller would do it at 1 MHz, a byte about every
9 µs, and waits while the MCU holds SCL (its slave stretches the clock until its
interrupt handler gives the next byte, or makes room). They are done one after another,
in the order sent.

## From the emulator

```
hello flipperoneemu host 1
int 0
```

`hello` comes first; then CPU_INT's level on connecting and at each change: `int 0` is
asserted (the MCU has something: read the status register, 0x0000), `int 1` let go.

```
ok
regs 0080 0001
got 01 00
nack
timeout
error …
```

The reply to each transfer, in order: `nack` when nothing answered at 0x69 (the MCU's
I2C1 is not in slave mode yet: its firmware sets it up about 0.6 s after starting),
`timeout` when SCL was held for 200 ms, `error` for a line it did not understand.

## Trying it

```sh
socat - UNIX-CONNECT:devices/one/host.sock
read 0080 1          # the intercom's version: regs 0080 0001
write 0040 0003      # the CPU is online: the screen waits for the CPU's frames
write 0310 f800      # LINK1 red (RGB 565)
write 0400 8f32      # play haptic effect 15
```
