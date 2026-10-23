# TODO: features to add

Planned features and build options. No implementation yet.

## Configure flag for maximum file read/write size

Today the file size limits are fixed at compile time in `main.c` (`TOOL_READ_MAX` = 64 KiB, `TOOL_EDIT_MAX` = 1 MiB; 8 KiB and 30 KB on 16-bit DOS), and `write_file` has no explicit cap of its own beyond what fits in a single request and buffer.

- Add a `configure` flag (e.g. `--with-max-file-size=BYTES`, or separate read/write/edit flags) that sets the maximum file size gork may read and write through `configure.ac` (`AC_ARG_WITH` passing `-DTOOL_READ_MAX=...` / `-DTOOL_WRITE_MAX=...` / `-DTOOL_EDIT_MAX=...`).
- Keep the current defaults when the flag is not given, and let `-DTOOL_READ_MAX=...` work the same way for systems built by hand without `configure`.
- Enforce the configured write ceiling in `tool_write_file` (and on the resulting file in `tool_edit_file`).

## Configure flag to enable Unicode support

Today UTF-8 on Unix requires passing `LIBS=-lncursesw` to `configure` by hand (`docs/BUILD.md`), `setlocale(LC_ALL, "")` is not called at startup, and the TUI wraps chat lines by byte count rather than character/column width (`tui.c`).

- Add a `configure` flag (`--enable-unicode`) in `configure.ac`.
- When enabled:
  - Search for `ncursesw` (and `<ncursesw/curses.h>` or `<curses.h>`) before plain `ncurses`/`curses`.
  - Define a macro (e.g. `-DGORK_UNICODE`) so startup calls `setlocale(LC_ALL, "")` before `newterm()`, which `ncursesw` needs in order to emit UTF-8 instead of `~`/`^` or M-notation.
  - Wrap and slice TUI lines on UTF-8 character/column boundaries rather than raw byte counts so multi-byte sequences are not split across rows.
- Leave it off (or opt-in) by default so old systems without `<locale.h>`, wide curses, or UTF-8 locales keep building unchanged.
