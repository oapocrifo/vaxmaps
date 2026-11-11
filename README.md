# Vax Maps

Infinite canvas for RPG maps: many large maps on one page, each with its own square or hex grid, rulers that count squares or hexes, links that jump between connected maps, free drawing, text and notes, layers and zones. C++17 + Qt 6 Widgets.

## Use

The window has no bars or fixed panels. Right click on the canvas opens the menu with every tool and command. Right click on a selected item (zone, map, link, text, note, stroke) opens its properties.

| Key | Tool |
|---|---|
| V | Select and move (Del deletes) |
| H, middle button, Space + drag | Pan |
| Mouse wheel | Zoom at the cursor |
| P / E | Pen / Eraser |
| T / N | Text / Note (double click edits) |
| M | Ruler |
| K | Calibrate a map's grid (F detects it, arrows nudge, Enter applies, Esc cancels) |
| R / O / G | Rectangle / circle / vertex zone (Shift keeps it square/round) |
| J | New link between maps |
| Shift+J | Show or hide links |
| S | Snap to the calibrated grid |
| L | Show or hide the layers panel |
| F11 | Full screen |
| Esc | Closes the open popup, clears every ruler |

- **Import maps:** drop files on the window or Ctrl+I. PNG, JPG, WebP, TIFF, BMP, GIF.
- **Calibrate** (one calibration per map, so dungeon maps in squares and a surface map in hexes live on the same canvas): select the map and press K, or press K and click the map. Pick Squares or Hexes. Squares: click a corner of a square in the highlighted quarter, then the next corner to its right. Hexes: click the centre of a hex, then the centre of the hex next to it on the right (the angle tells pointy-top from flat-top). The grid is matched to the map's own lines at once. The next quarter is then outlined and you can measure there to check it, up to 4 steps, or press Enter at any step. F finds squares or hexes by itself. The yellow grid is the preview: arrows move it (Shift = 0.1 px), Enter applies, Esc cancels. After applying, the grid is hidden. "Show the calibrated grid" in the map's properties shows it.
- **Uneven grids:** the detection measures the grid in each quarter of the map and then region by region (about 5 cells each), so the grid follows maps whose squares or hexes change size or drift across the map. The map's properties say "follows local variations" when that happened.
- **Ruler:** drag to measure. The count is in squares or hexes of the map under the segment's start (squares: a diagonal counts 1, as in D&D 5e). Drag again from a ruler's end to add a turn (each turn shows its own segment, the end shows the total), or drag elsewhere for another ruler. Esc clears all rulers.
- **Links:** with J, drag an area (stairs, a door), then drag its pair, which can be on another map. Links live in the Links layer, always on top, and are invisible. A click on one moves the view to its pair at the same zoom, keeping the clicked spot at the same place on screen. Shift+J (or the Show box of the Links layer) shows them so you can select, move, resize, rename or delete them. Deleting one end deletes both.
- **Zones:** select to see the handles. White squares on corners and sides resize (Shift on a corner keeps the proportions). On polygons the blue dots move vertices, a double click on an edge adds one and a right click on a vertex removes it. Name, colour and description are in the right-click popup.
- **Layers:** new items go to the active layer (highlighted). "Lock" blocks selecting and moving, and clicks fall through to the layers below. Ctrl+M moves the selection to the active layer.

## Performance (techniques from PureRef and Atlas VTT)

- On import each map becomes a pyramid of 512 px tiles (level 0 = original size, each level half the previous one), stored in a disk cache. Only the smallest level stays in memory.
- The level drawn follows the zoom. Only visible tiles are loaded, in threads, newest request first. While a tile loads, the closest coarser level already in memory is shown.
- Decoded tiles share a 1 GB LRU cache across all maps.
- Tiles are lossless PNG: at full zoom the pixels are the original ones. Each tile carries 1 px of its neighbours so there are no seams.
- Cache in `%LOCALAPPDATA%\VaxMaps\Vax Maps\cache\tiles2`. A textured 6000 x 4000 px map took 32 MB in the test. It can be deleted while the program is closed.

## `.vaxmap` project

JSON with layers and items. Maps are referenced by path (relative to the project when possible), not embedded: keep the images with the project when copying it. Projects from 0.3 open with their calibration converted to a square grid.

## Build

Windows (Qt 6.4+ with MinGW or MSVC, CMake):

```
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.x/mingw_64
cmake --build build --config Release
```

Linux: `cmake -S . -B build && cmake --build build`.

Tests: `VaxMaps --selftest <folder> [--bench]` (on Linux without a screen: `QT_QPA_PLATFORM=offscreen`).
The distributed `.exe` is cross-built on Linux with static Qt 6.4.2 and MinGW: see `tools/`.

## New in 0.6

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

## New in 0.5

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

## Changes in 0.4.1

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

## Licence note

The manual calibration flow and two small formulas (cell size from two corners, circular mean of the clicks) follow Atlas VTT, which is AGPL-3.0. If you distribute Vax Maps, publish it under AGPL-3.0.
