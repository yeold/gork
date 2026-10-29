# gork over a serial line

For a machine with no network at all: no Ethernet card, no TCP/IP stack,
no IPX. Built `--with-serial`, gork sends everything over a serial port to
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

## Build

```sh
./configure --with-serial
make
```

It needs `termios.h`. It cannot be combined with `--with-openssl`: the
relay does the TLS.

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
