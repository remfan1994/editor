EDITOR 0.6.14-r4
Word wrapping revision of the selective 0.6.14 rollback.
Design by remfan1994; published by ThatIAm.vip.

Build:
  cc -std=c99 -O2 -Wall -Wextra -pedantic editor.c -o editor

Build without external command execution:
  cc -std=c99 -O2 -Wall -Wextra -pedantic -DEDITOR_NO_CMD editor.c -o editor

Or use the normal executable with --no-cmd for a single launch.
This is not an OS sandbox. See ./editor --help and ./editor --manual.

WORD WRAPPING
  On by default when no saved preference exists.
  Esc -> Help -> Word wrap, or Esc -> Help -> Settings -> Word wrap.
  In command mode: toggle-wrap
  The toggle applies immediately and saves wrap=on/off in the existing
  config file (~/.config/editor/config or $XDG_CONFIG_HOME/editor/config).
  settings show reports both saved and current values.
  editor --wrap FILE or editor --no-wrap FILE overrides one launch only.
  No-wrap restores horizontal scrolling. Soft wrap changes only display;
  no file bytes, line numbers, or undo records are changed by the toggle.
  Up/Down and Page Up/Down follow visible rows when wrapping is enabled.
  Home/End, goto and line-based editing commands use logical file lines.
  Resize reflows the display; tabs expand relative to each visible row.

Install on Linux/Crostini or BSD (per user):
  mkdir -p "$HOME/.local/bin"
  install -m 755 editor "$HOME/.local/bin/editor"
  "$HOME/.local/bin/editor" --version
  Ensure $HOME/.local/bin precedes conflicting system commands in PATH.

No new runtime dependencies or document formats. CHANGES.txt and
VALIDATION.txt describe r4. Previous reports and patches are historical.
The incremental patch editor-r3-to-r4.patch applies to exact r3 editor.c:
  patch -p0 < editor-r3-to-r4.patch
Nothing here deletes existing source, state or repositories.

Tests (Python 3 and pexpect needed for PTY testing only):
  sh tests/run-wrap.sh
  sh tests/run-stability.sh
  sh tests/run-interim.sh
