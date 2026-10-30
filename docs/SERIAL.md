# gork over a serial line

For a machine with no network at all: no Ethernet card, no TCP/IP stack,
no IPX. Built `--with-serial`, or from a `-nonet` package, gork sends
everything over a serial port to
[gork-relay](https://github.com/yeold/gork-relay) on another machine,
which passes it on to the API over TLS.

```
+----------+   null modem   +----------------+     TLS     +--------------+
|   gork   |--- RS-232 ---->|  gork-relay -S |------------>| /v1/messages |
+----------+                +----------------+             +--------------+
```

A serial build has no network code at all, so it builds on systems with no
`netdb.h` or socket library. It talks over the line only: no TCP relay,
and no remote (`"url"`) MCP servers. MCP servers that run as local
commands still work.

## Build, and the name of the port

| System | Package, or how to build it | `DEVICE` |
|---|---|---|
| Unix | `./configure --with-serial && make`. Needs `termios.h`; cannot be combined with `--with-openssl` (the relay does the TLS) | `/dev/ttyS0`, `/dev/ttyUSB0`, ... |
| DOS, 32-bit | `gork-<version>-dos-nonet.zip`, or `make -f Makefile.dj gork-nonet.exe` | `COM1` to `COM4` |
| DOS, 16-bit | `gork-<version>-dos16-nonet.zip` (`dos16build.sh`) | `COM1` to `COM4` |
| Windows 95, NT 3.1 and later | `gork-<version>-win32-legacy-nonet.zip`, or `make -f Makefile.w32 gork-nonet.exe gorknt3-nonet.exe` | `COM1`, `COM2`, ... |
| UEFI | `gork-<version>-uefi-nonet.zip` | `COM1`, `COM2`, ...: the firmware's serial ports, in its order |
| OS/2 | not packaged: add `-DGORK_SERIAL serline.c` to the gcc line in [build/os2.md](build/os2.md), and leave out `-lsocket` | `COM1`, `COM2`, ... |

None of them needs a network library: the DOS ones run with no packet
driver, and are smaller for it.

On DOS gork drives the UART itself (an 8250 or 16550, at the address the
BIOS has for the port), and takes what comes in on its interrupt, IRQ 4
for COM1 and COM3, IRQ 3 for COM2 and COM4. The speeds there are those
that divide 115200. Under Windows 3.x the DOS box shares the port with
Windows; nothing else may use it at the same time.

On UEFI the firmware's console is often on the first serial port: give
gork another one, or the console's output ends up in the frames.

## Wiring

A null-modem cable (or two USB serial adapters and one) between a serial
port on each machine. For a test without hardware, a pair of
pseudo-terminals does the same:

```sh
socat pty,raw,echo=0,link=/tmp/ttyA pty,raw,echo=0,link=/tmp/ttyB
```

## Run

On the machine with the network, gork-relay 0.4 or later:

```sh
gork-relay -t 0 -S /dev/ttyS0:38400 api.anthropic.com
```

(`-t 0`: no TCP listener; leave it out to take TCP clients as well.) On
the machine with gork, in `~/.gork.conf`:

```ini
relay   = serial:/dev/ttyS0:38400
api_key = sk-ant-...
```

`relay = serial:DEVICE[:BAUD[:rtscts]]`. The speed must be the same at
both ends. It is 9600 unless given; the speeds are 1200 to 38400, and 57600, 115200 and
230400 where the system has them. Add `:rtscts`, at both ends, for
hardware flow control, if the cable carries RTS and CTS. The line is 8
data bits, no parity, one stop bit, and modem control lines are ignored.

A request is sent whole, and so is its reply: a conversation of 30 KB
takes about 30 s each way at 9600 baud, and 8 s at 38400. Raise `timeout`
(120 s) to suit the line.

## What goes over the line

Frames, both ways, in SLIP's framing:

```
END  TYPE ID DATA SUM(2)  END
```

- `END` is 0xC0. Inside a frame, a 0xC0 byte is sent as 0xDB 0xDC, and a
  0xDB byte as 0xDB 0xDD.
- `TYPE` is `O` (open), `D` (data) or `C` (close).
- `ID` is the request's number, one byte. gork starts from a number of its
  own and counts up.
- `DATA` is at most 1024 bytes.
- `SUM` is a Fletcher-16 (both sums modulo 255) of `TYPE`, `ID` and
  `DATA`, the second sum first.

A request is an `O`, then the HTTP request in `D`s. The relay connects to
the API for it and sends the reply back in `D`s of the same `ID`, then a
`C` when the API closes. One request at a time: an `O` drops whatever the
relay was doing for an earlier one. A `C` from gork abandons its request
(Esc in the TUI, or a timeout).

Nothing is sent again. A damaged frame, one whose sum is wrong, fails the
request at either end. gork then tries again as it does after a network
error (`retries`), with a new `ID`. Frames of another `ID` are left over
from an abandoned request, and ignored. Since damage ends at the next
`END`, the frames after a damaged one are read as usual.

## Tests

`python3 testing/serialtest.py ./gork` runs a serial build over a
pseudo-terminal, against a relay played by the script: a tool round trip,
replies of many frames, frames left over from an earlier request, a
damaged frame, a timeout. gork-relay's side has its own checks, in its
`test/relaytest.py`.

The `-nonet` packages run their system's usual checks over the line,
with a real `gork-relay -S` (`$GORK_RELAY`) in front of the fake API:
`testing/dostest.py` takes a `-nonet` zip and wires QEMU's COM1 to it;
`GORK_NONET=1 testing/wintest.py` gives Wine a COM1 at one end of a
`socat` pair; `testing/uefitest.py` runs a `-nonet` package on x64 over
COM2, as OVMF's console is on COM1. The OS/2 build was run by hand on
Warp 4.52 in QEMU, its COM1 on a socket.
