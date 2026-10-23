# TODO: features to add

Planned features and build options. No implementation yet.

## Configure flag for maximum file read/write size

Today the file size limits are fixed at compile time in `main.c` (`TOOL_READ_MAX` = 64 KiB, `TOOL_EDIT_MAX` = 1 MiB; 8 KiB and 30 KB on 16-bit DOS), and `write_file` has no explicit cap of its own beyond what fits in a single request and buffer.

- Add a `configure` flag (e.g. `--with-max-file-size=BYTES`, or separate read/write/edit flags) that sets the maximum file size gork may read and write through `configure.ac` (`AC_ARG_WITH` passing `-DTOOL_READ_MAX=...` / `-DTOOL_WRITE_MAX=...` / `-DTOOL_EDIT_MAX=...`).
- Keep the current defaults when the flag is not given, and let `-DTOOL_READ_MAX=...` work the same way for systems built by hand without `configure`.
- Enforce the configured write ceiling in `tool_write_file` (and on the resulting file in `tool_edit_file`).

## Configure flag and autodetection for Unicode support

Today UTF-8 on Unix requires passing `LIBS=-lncursesw` to `configure` by hand (`docs/BUILD.md`), `setlocale(LC_ALL, "")` is not called at startup, and the TUI wraps chat lines by byte count rather than character/column width (`tui.c`).

- Have `configure` (`configure.ac`) autodetect whether the system supports Unicode (`ncursesw` with `<ncursesw/curses.h>` or `<curses.h>`, plus `<locale.h>` / `setlocale`), and fall back cleanly to plain ASCII / 8-bit `ncurses`/`curses` when it does not.
- Provide `--enable-unicode` / `--disable-unicode` (`--enable-unicode=auto` by default) so builds can also force it on or off explicitly.
- When Unicode support is active:
  - Link against `ncursesw` (unless static or missing, in which case `auto` falls back to `ncurses`/`curses`).
  - Define `-DGORK_UNICODE` so startup calls `setlocale(LC_ALL, "")` before `newterm()`, which `ncursesw` needs in order to emit UTF-8 instead of `~`/`^` or M-notation.
  - Wrap and slice TUI lines on UTF-8 character/column boundaries rather than raw byte counts so multi-byte sequences are not split across rows.
- When the system lacks `<locale.h>`, wide curses, or UTF-8 locales (or `--disable-unicode` is passed), fall back to the current ASCII / single-byte behavior unchanged.

## Show target path and file name on file operations

Today `run_tools()` prints only `[tool] <name>` (`[tool] read_file`, `[tool] write_file`, `[tool] edit_file`, `[tool] list_dir`), while `run_command` additionally prints `[run] <command>` on `stderr` so the user can see what is being run in both batch mode and the TUI (`tui.c` styles `[tool]` and `[run]` lines with `ST_TOOL`).

- Have the file tools (`read_file`, `write_file`, `edit_file`, `list_dir`) print the target path and file name (either in the `[tool] <name> <path>` line in `run_tools()` or with a `[...]` diagnostic line like `run_command`'s `[run] <command>`) so the user can see which file or directory each operation touches without needing an approval prompt.
