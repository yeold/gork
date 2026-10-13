# Contributing

Bug reports, results from systems nobody has tried, and patches are all
welcome.

## Reports

gork is for old and obscure systems, so "I built it on X and this
happened" is a useful report whether it worked or not. Say:

- the system and its version, the compiler and the curses library;
- how it was built (`configure` and `make`, or the one-line compiler
  command from [docs/BUILD.md](docs/BUILD.md)) and any warnings;
- what worked: `gork --version`, a batch task, the TUI;
- if `testing/looptest.py` could run there, what it printed.

[docs/TODO-OSES.md](docs/TODO-OSES.md) lists the systems still to try.

For a bug, give the command line, the config with the key removed, what
you expected and what happened. `GORK_TRACE=1` shows the reply headers.
**Leave your API key out of anything you post.**

## Patches

- gork is C89 and has to stay that: it is compiled by gcc 2.7 and by
  vendor compilers of the 1990s. No `//` comments, no declarations after
  statements, no `snprintf`, nothing newer than what the oldest system in
  the README's table has. `configure` builds with
  `-std=gnu89 -pedantic -Wall -W`; a patch should add no warnings.
- Keep to the style of the file you are changing, and write comments
  that say why.
- No new dependencies. gork needs a C compiler, curses and sockets, and
  the vendored cJSON; that is the point of it.
- What differs between systems goes behind `port.h` or an `#ifdef` with a
  comment naming the system that needs it.
- Run the checks that cover your change, and say in the commit message or
  the pull request which ones you ran and where:

  ```sh
  python3 testing/looptest.py ./gork          # Unix
  python3 testing/dostest.py                  # the DOS build, in QEMU
  python3 testing/wintest.py wine gork.exe    # the Windows build
  ```

  A change to the loop, the tools or the transport should come with a
  check. "Not tested on DOS" is a fine thing to say; an untested claim
  that something works is not.
- One change per commit, with a message that says what it does and why.
- Update the README or [docs/BUILD.md](docs/BUILD.md) when behaviour or a
  build step changes.

The relay has its own repository:
[gork-relay](https://github.com/yeold/gork-relay).

## Licence

gork is under the GNU General Public License, version 3 or later (see
[LICENSE](LICENSE)). By sending a change you agree that it is published
under the same licence, and state that you have the right to submit it.
