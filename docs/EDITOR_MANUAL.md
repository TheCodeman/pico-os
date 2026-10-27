# Pico 2 mini OS Text Editor

The full-screen text editor: `edit [file]` (or `ed`). It edits plain text files on the USB drive or a network share, such as programs, scripts and /AUTORUN.

## The editor

`edit file` opens a file; a name that doesn't exist yet starts a new file.

The screen has a title row, 28 text rows and a status row. The status row
shows the line and column, and whether the file uses CRLF or LF line
endings. The editor keeps whichever the file had.

![edit](editor.png)

Above: `edit /NET/examples/NOVA.BAS`, a BASIC game from the examples
(opened over a network share), after Ctrl-G 150. The bottom row lists the
main keys.

| Key | Action |
|---|---|
| Arrows, PgUp / PgDn, Home / End | Move |
| Backspace / Delete | Delete |
| Enter | New line, indented like the line above |
| Tab | Insert a tab (shown 8 columns wide) |
| Ctrl-S | Save |
| Ctrl-Q | Quit; with unsaved changes it asks: `y` save, `n` discard, Esc go back |
| Ctrl-F | Find |
| F3 | Find next |
| Ctrl-G | Go to a line number |
| Ctrl-K | Cut the line (repeat to cut several) |
| Ctrl-U | Paste the cut lines |
| Ctrl-L | Redraw the screen |

**Finding:** Ctrl-F asks for the text on the bottom row; Enter goes to
the next place it appears, and F3 finds the one after.

![find](editor_find.png)

**Quitting with changes:** the title row says `[modified]` once the text
has changed, and Ctrl-Q asks whether to save: `y` saves and quits, `n`
quits without saving, Esc goes back to editing.

![save](editor_save.png)

Limits: files up to 64 KB, cut buffer 8 KB.
