# The Launcher

[Documentation](../README.md) > [User Manual](index.md) > **The Launcher**

---

The launcher is the home window of the boards with a screen (the M5Stack Tab5, the ESP32-P4-LCD-7B, the RP2350-LCD-7, the VisionFive 2 on HDMI, and QEMU's RAM screen). It has two faces, chosen by the tab bar at its top: **Shortcuts** and **Files**. The face last used comes back the next time.

Open it with `launcher` in the shell, a tap or click on **LugalOS** at the left of the menu bar, or `Cmd+Space` from anywhere. It comes up by itself when a touch screen is the only way in (a keyboard unplugged).

## Shortcuts

An icon grid of the things you start most: applications, documents and folders. A tap or click opens one -- an application starts (or comes to the front if it runs already), a document opens in its application, a folder opens in the Files face.

A long press, a right click, `Shift+Enter` or `m` on a tile gives its actions:

| Action | What it does |
| :--- | :--- |
| Open | as a tap |
| Rename | a dialog with the name selected: type the new one, Enter (or Rename) |
| Remove | asks first; the shortcut goes, what it opens stays where it is |
| Move Left / Move Right | its place in the grid |

The first time, the shortcuts are the defaults: Settings, Terminal, the games this board has, the Help, and the Documents folder. Add your own from the Files face (Add to Shortcuts).

They are kept in `/sd0/system/shortcuts` (`/flash0/system/shortcuts` on a board without a card), one per line: a type (`app`, `doc`, `dir` or `book`), the target (an application's id or a path), the label and an icon, separated by tabs. The icon is a built-in icon's name, empty for the target's own. The file can be edited by hand, or over 9P; the launcher reads it again each time it opens. In the shell, `shortcuts` lists, adds, removes, moves, renames and resets them (see [the shell](shell.md)).

In a narrow window the grid has fewer columns; below two it becomes a list.

## Files

The mounted volumes first -- `flash0`, `sd0`, `ram0`, `nvme0` on the VisionFive 2, and any 9P mount -- then the folders and files under them. Above the list a path bar: each part of the path can be tapped to go back to it, the first one (`/`) to the volumes.

Each row shows an icon by the file's type, its name, and on the right its size (or `folder`, or a volume's description). A Markdown file is shown by its first heading, its file name on the right. Folders come first, then files, both by name, numbers in names in their order (`file2` before `file10`).

A tap or click on a folder opens it; on a file, opens it in its application (the reader for Markdown and Gemtext, the editor for text and code). A long press, a right click or `Shift+Enter` gives:

| Action | What it does |
| :--- | :--- |
| Open | as a tap |
| Open in Editor / Open in Reader | the file's other application |
| Add to Shortcuts | a tile for it, named by its heading or its name |
| Info | its path, size, heading and application |
| Delete | asks first, then removes the file (not folders) |

A directory of up to 512 entries is listed whole; a larger one shows its first 512 and says so.

## Keys

| Key | Where | What it does |
| :--- | :--- | :--- |
| `Ctrl+Tab`, `Ctrl+Shift+Tab` | both | the other face |
| `Tab`, `Shift+Tab`, arrows | both | move the focus; the first key only shows where it is |
| `Enter`, `Space` | both | open the focused item |
| `Shift+Enter` | both | the focused item's actions |
| `m` | Shortcuts | the focused tile's actions |
| a letter | Files | the next entry whose name starts with it |
| `Backspace`, `Esc`, `Left` on a row | Files | up one folder (from a volume's top: the volumes) |
| `PgUp`, `PgDn`, `Home`, `End` | Files | the focus a page up or down, to the first or the last entry |
| `Cmd+Space` | anywhere | the launcher |
| `Cmd+,` | anywhere | the Settings app |

## The control gallery

`launcher gallery` shows every control of the toolkit the launcher is built with -- tabs, toggles, a slider, a path bar, a text field, dialogs, action sheets, the icons -- to try by touch, mouse and keys. Done returns to the launcher.
