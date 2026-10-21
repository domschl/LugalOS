# LugalOS Text Editors

LugalOS provides two built-in text editors designed for different interaction environments:
1. **`e`**: An interactive full-screen editor with classic Emacs keybindings, designed for display terminals (RP2350-LCD-7, ESP32-P4-LCD-7B) and VT100-compatible serial terminals.
2. **`ed`**: A standard teletype line editor, ideal for low-bandwidth serial lines, teletypes, or scripting.

---

## 1. Full-Screen Editor (`e`)

To launch the full-screen editor:
```text
lsh> e /sd0/hello.c
```
If the file exists, it will be loaded into the working buffer. If no argument is given, a new empty buffer is created.

### Dynamic Memory Management
The editor dynamically allocates its text buffer from the physical page heap on entry and releases all memory back to `palloc` immediately upon exiting, ensuring zero permanent SRAM footprint.

### Keybinding Reference

#### Cursor Movement
| Keybinding | Function |
| :--- | :--- |
| `Ctrl+P` or `Up Arrow` | Move cursor up one line |
| `Ctrl+N` or `Down Arrow` | Move cursor down one line |
| `Ctrl+B` or `Left Arrow` | Move cursor left one character |
| `Ctrl+F` or `Right Arrow` | Move cursor right one character |
| `Ctrl+A` or `Home` | Move to beginning of current line |
| `Ctrl+E` or `End` | Move to end of current line |
| `Ctrl+V` or `Page Down` | Scroll forward one screen |
| `Alt+V` or `Page Up` | Scroll backward one screen |
| `Alt+<` | Move to beginning of buffer |
| `Alt+>` | Move to end of buffer |

#### Text Editing & Clipboard
| Keybinding | Function |
| :--- | :--- |
| `Backspace` or `Ctrl+H` | Delete character before cursor |
| `Ctrl+D` or `Delete` | Delete character under cursor |
| `Ctrl+K` | Kill (cut) from cursor to end of line |
| `Ctrl+Y` | Yank (paste) the most recently killed text |
| `Enter` | Insert newline |
| `Tab` | Insert spaces / tab indentation |

#### File Operations & Exit
| Keybinding | Function |
| :--- | :--- |
| `Ctrl+X Ctrl+S` | Save current buffer to disk |
| `Ctrl+X Ctrl+W` | Write buffer to a new filename (Save As) |
| `Ctrl+X Ctrl+C` | Exit editor (prompts if unsaved changes exist) |
| `Ctrl+G` | Cancel current command or search prompt |

#### Search
| Keybinding | Function |
| :--- | :--- |
| `Ctrl+S` | Incremental forward search (enter search string in status bar) |

---

## 2. Teletype Line Editor (`ed`)

For environments where ANSI cursor addressing is unavailable or over noisy serial lines, LugalOS includes `ed`:

```text
lsh> ed /sd0/notes.txt
```

### Basic Commands
* `a`: Append text after current line (terminate input by typing `.` on an empty line).
* `i`: Insert text before current line.
* `p`: Print current line (or range, e.g. `1,$p` to print all lines).
* `n`: Print lines preceded by line numbers (`1,$n`).
* `d`: Delete current line (or range, e.g. `3,5d`).
* `s/old/new/`: Substitute string on current line (`1,$s/old/new/g` across all lines).
* `w [file]`: Write buffer to disk.
* `q`: Quit editor (will warn if unsaved changes exist; use `q` twice to force exit).

---

## 3. Recommended Workflow

* Use **`e`** for writing C programs ([C programming](c_programming.md)), modifying Lisp scripts, and interactive editing on terminals.
* Use **`ed`** when connecting over low-baud wireless or raw UART serial links.
