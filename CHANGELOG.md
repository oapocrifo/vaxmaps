# Changelog

## 0.7

Projects from 0.6.x open unchanged. Tokens with vision saved by 0.7 open in 0.6.x without it.

- **Token vision**: in a token's properties, **Vision**: a distance in the units of the map under the token, and **Circle** or **Cone**. The fog of every map inside it clears when the token is placed, along the way while it is dragged, and when its vision changes. A cone has a width (10° to 360°) and a direction: a slider, or **[** and **]** with the token selected (15° per press, 45° with Shift). Undo covers the fog again.
- **Walls**: a pen stroke marked **Wall: blocks token vision** (right click it) stops the vision of tokens at its line. The setting applies to every selected stroke and is saved with the project.
- Fixed: automatic detection (F) took hex maps for squares, or for hexes √3 times too big. The slanted edges of pointy-top hexes lined up as flat-top hexes √3 times wider, and the search for hexes stopped at cells 1/7 of a quarter of the map. Line evidence is now measured only along each direction, the denser of two grids that both fit wins, and hexes are searched up to the same size as squares. On 48 generated hex maps filled with terrain colours (hexes 30 to 150 px wide, edges 1 to 5 px, faint and strong) it now finds 43. 0.6.2 found 3 of a similar set. The 5 missed are hexes 30 px wide with faint or 5 px edges.
- Detection asks for stronger evidence before it reports a grid on a map with walls and no grid.

## 0.6.2

Fixes and maintenance. Projects from 0.6 and 0.6.1 open unchanged.

- Security: the Windows build uses Qt 6.8.5 LTS instead of 6.4.2. The image libraries inside the old one had known flaws that a crafted map file could exploit: libwebp 1.2.4 (CVE-2023-4863, WebP) and libtiff 4.4.0. Now libwebp 1.6.0 and libtiff 4.7.1.
- Maps are prepared faster on import: the tiles are compressed on several cores. 8 maps of 6000 x 4000 px took 9.2 s instead of 15.9 s on 2 cores.
- A project saved by a newer version of Vax Maps opens with a warning. What this version does not know is left out, and Save asks for a new name, so the original is not overwritten without it.
- The tile cache no longer grows forever: at start, the pyramids of maps not opened for 90 days are deleted in the background.
- Save package refuses projects whose images add up to more than 4 GB, with a message. Before, the .zip came out broken.
- Command line and dropping files on `VaxMaps.exe`: a package (.zip) or map images now open too, not only .vaxmap. A project given while a recovery copy is restored is no longer ignored: the Save question decides which one stays.
- GitHub Actions builds the program and runs the self-test on every push.

## 0.6.1

- Portable copy (`VaxMaps-0.6.1-portable.zip`): with a `VaxMaps-data` folder beside `VaxMaps.exe`, the settings, tile cache and recovery copy are kept in it, and nothing is written to the registry or the user's folders.

## 0.6

- **Tokens** (B): a click puts a numbered disc on the square clicked. Dragged, it lands on the centre of a square (on a corner when it is 2x2 or 4x4). Right click it for its name, colour and size (1x1 to 4x4). A ruler started on a token measures from its centre.
- **Fog of war**, for the game master's screen: in a map's properties, **Cover the map with fog**. The **fog brush** (F) reveals where it passes, Alt + drag covers again. Its size is 10 times the pen width. **Opaque fog** in the menu switches between hiding the map and only darkening it; the choice is saved with the project.
- **Find** (Ctrl+F) also looks in zone descriptions, in texts and notes, and in token names.
- **Cell size typed in**: in the calibration panel, type the cell size in px and press **Use**. The arrows move the grid before Enter applies it. Hexes typed in are pointy-top.
- **Diagonal rule** per map, in its properties: a diagonal counts 1 (D&D 5e), 1-2-1 (5/10/5 ft), or the straight-line distance.
- The fit of a measured grid to the map's lines runs in the background: the window no longer stops for 0.5 to 1 s after each measurement on a big map.
- **Save package…**: the project and all its images in one .zip. Opening the .zip (Open, drop, double click) extracts it to a new folder beside it and opens the project.
- **Export full size…**: the selection, or the visible area, at the maps' own resolution (at most 100 megapixels).
- The window, tool, colour, width, the Layers panel and the minimap are as they were at the last close.
- **Shortcuts** (F1): every key and mouse gesture.
- **Minimap** in the lower left corner: the whole project and the visible area. A click or a drag moves the view.
- Program icon, also for .vaxmap files.
- **Installer** (`VaxMaps-0.6.0-setup.exe`): installs for the current user only (no administrator rights) in `%LOCALAPPDATA%\Programs\Vax Maps`, adds a Start menu entry, and makes a double click on a .vaxmap open it. Uninstall from Windows Settings. The program is not signed: Windows SmartScreen warns the first time.

## 0.5

- **Undo and redo**: Ctrl+Z, Ctrl+Y (or Ctrl+Shift+Z), 100 steps. Every edit counts, a deleted map comes back with its calibration. Edits less than 0.4 s apart are one step. A step reuses the strokes that did not change: with 20 000 strokes it takes 3 ms and almost no memory.
- **Recovery copy**: while there are unsaved changes the project is copied every minute to `recovery.vaxmap` in the program's data folder. After a crash or a power cut the next start offers to restore it. Saving, discarding or closing normally removes the copy.
- **Missing images**: a project whose images moved asks for the first one when it is opened and finds the others in the same folder. Later: right click the selected map, **Locate image…**.
- **Clear cache** (menu, Project): deletes the tile cache of the maps not opened since the program started. Use it with a single window of the program open.
- **Text size**: slider in the properties of a text or note.
- **Copy, paste, duplicate**: Ctrl+C, Ctrl+V (at the pointer), Ctrl+D, for zones, texts, notes, strokes and maps (with their calibration). Links are not copied.
- **Arrows** move the selection 1 px on screen, 10 px with Shift. With nothing selected they scroll the view, as before.
- **Several maps imported at once** land in rows and columns instead of one long row.
- **Open recent…** (menu): the last 5 projects.
- **Find…** (Ctrl+F): type part of the name of a zone or link, Enter or a click moves the view there.
- **Export view…** (Ctrl+E): the visible canvas as PNG or JPEG, at screen resolution, without selection marks.
- **Portuguese**: the interface follows the system language at the first start. The **Português / English** button in the menu switches it. Number fields use the decimal comma in Portuguese.
- On a window lower than the menu, the mouse wheel over the menu scrolls it.
- Fixed: the properties popup opened again at once (a second right click on the item) came out empty.

## 0.4.1

Bug fixes, no new features. Projects from 0.4 open unchanged.

- New and Open clear the rulers of the previous project.
- Ctrl+M no longer moves a link out of the Links layer (the link was lost when the project was reopened).
- The cell size field in a map's properties changes the grid only when its value changes (clicking in and out of it dropped the local corrections and rounded the size).
- A click without a drag makes no zone or link at zoom 200 % or more.
- Right click on a polygon vertex removes that vertex also when the mouse moves during the click (the next vertex was the one that went).
- Double click adds a polygon vertex only within 10 px of an edge, not anywhere inside the zone.
- Right click on empty canvas keeps the selection, so Delete and Move to the active layer in the menu act on it.
- Right click on a selected item opens its properties also where a hidden link or a ruler label lies over it.
- The wheel zooms at the cursor right after a pan (the view jumped by the pan distance), and zooms in again after Fit all on very wide content.
- Space held while the window loses the focus no longer leaves pan mode on.
- A link end saved without its pair gets it with New link (J) after the project is reopened.
- A link without area (a click made one, see above) no longer throws the view to the edge of the canvas.
- An image that was missing loads when the project is opened again, without restarting the program.
- Closing the program no longer waits for the maps still being prepared (8 s with four 9000 x 7000 px maps in the test).
- Esc in the colour dialog closes the dialog, not the popup under it. The dialog's title is in English.
- Tool keys work after a layer is renamed.
- Text selected during an edit is not left highlighted. A text with nothing typed is not saved.
- Dropped files are handled after the drop returns, so the Save question does not freeze the window they came from.
- Two memory errors: the Show box of the Links layer (use after free), closing the window with an item selected.
- `tools/build-windows.sh` finds the Qt it has just built (it uses qt-cmake).

