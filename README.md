# Vax Maps

Infinite canvas for tabletop RPG maps. Many large maps on one page, each with its own square or hex grid, rulers that count squares or hexes, links that jump between connected maps, tokens, fog of war, free drawing, text, notes, layers and zones.

C++17, Qt 6 Widgets. Windows and Linux. Current version: 0.6.2.

## Download

From the Releases page:

- `VaxMaps-0.6.2-setup.exe`: installs for the current user (no administrator rights) in `%LOCALAPPDATA%\Programs\Vax Maps`, adds a Start menu entry and opens `.vaxmap` files on double click. Uninstall from Windows Settings.
- `VaxMaps-0.6.2-portable.zip`: runs from any folder or USB drive. See [Portable copy](#portable-copy).

The program is not signed. Windows SmartScreen warns on the first start.

## Quick start

1. Drop map images on the window, or Ctrl+I. PNG, JPG, WebP, TIFF, BMP, GIF.
2. Select a map, press K, then F to detect its grid. Enter applies.
3. Press M and drag to measure in squares or hexes.
4. Ctrl+S saves the project as `.vaxmap`.

The window has no toolbars. Right click on the canvas opens the menu with every tool and command. Right click on a selected item opens its properties. F1 lists every key and mouse gesture.

## Keys

| Key | Tool |
|---|---|
| V | Select and move (Del deletes) |
| H, middle button, Space + drag | Pan |
| Mouse wheel | Zoom at the cursor |
| P / E | Pen / Eraser |
| T / N | Text / Note (double click edits) |
| M | Ruler |
| K | Calibrate a map's grid (F detects it, arrows nudge, Enter applies, Esc cancels) |
| R / O / G | Rectangle / circle / vertex zone (Shift keeps it square or round) |
| J | New link between maps |
| Shift+J | Show or hide links |
| B | Token |
| F | Fog brush (Alt + drag covers again) |
| S | Snap to the calibrated grid |
| L | Show or hide the layers panel |
| Ctrl+N / Ctrl+O | New / Open |
| Ctrl+S / Ctrl+Shift+S | Save / Save as |
| Ctrl+I | Import maps |
| Ctrl+Q | Quit |
| Ctrl+Z / Ctrl+Y | Undo / Redo (100 steps) |
| Ctrl+C / Ctrl+V / Ctrl+D | Copy / Paste at the pointer / Duplicate |
| Ctrl+M | Move the selection to the active layer |
| Ctrl+F | Find |
| Ctrl+E | Export the view as PNG or JPEG |
| F1 | Shortcuts |
| F11 | Full screen |
| Esc | Closes the open popup, clears every ruler |

## Features

### Grid calibration

One calibration per map, so dungeon maps in squares and a surface map in hexes share the same canvas.

- Select the map and press K, or press K and click the map. Pick Squares or Hexes.
- Squares: click a corner of a square in the highlighted quarter, then the next corner to its right.
- Hexes: click the centre of a hex, then the centre of the hex to its right. The angle tells pointy-top from flat-top.
- The grid snaps to the map's own lines. The next quarter is then outlined so you can measure there to check, up to 4 steps. Enter applies at any step.
- F detects squares or hexes without clicks.
- The cell size can be typed in px in the calibration panel (Use button). Hexes typed in are pointy-top.
- The yellow grid is a preview. Arrows move it (Shift = 0.1 px), Enter applies, Esc cancels. After applying it is hidden. "Show the calibrated grid" in the map's properties shows it.
- Uneven grids: detection measures each quarter of the map and then regions of about 5 cells, so the grid follows maps whose cells change size or drift. The map's properties show "follows local variations" when this happened.

### Ruler

Drag to measure. The count uses the grid of the map under the segment's start. Drag from a ruler's end to add a turn. Each turn shows its segment and the end shows the total. Drag elsewhere for another ruler. A ruler started on a token measures from its centre.

Diagonal rule per map, in its properties: 1 (D&D 5e), 1-2-1 (5/10/5 ft), or straight-line distance.

### Links

With J, drag an area (stairs, a door), then drag its pair, on the same map or another. A click on a link moves the view to its pair at the same zoom, with the clicked spot at the same place on screen. Links live in the Links layer, always on top and invisible. Shift+J shows them for selecting, moving, resizing, renaming or deleting. Deleting one end deletes both.

### Tokens

B and a click puts a numbered disc on the clicked square. A dragged token lands on the centre of a square, or on a corner when it is 2x2 or 4x4. Right click for name, colour and size (1x1 to 4x4).

### Fog of war

In a map's properties: Cover the map with fog. The fog brush (F) reveals where it passes. Alt + drag covers again. Brush size is 10 times the pen width. "Opaque fog" in the menu switches between hiding the map and darkening it, saved with the project.

### Zones

Select a zone to see its handles. White squares on corners and sides resize (Shift on a corner keeps the proportions). On polygons the blue dots move vertices, a double click within 10 px of an edge adds one, a right click on a vertex removes it. Name, colour and description are in the right-click popup.

### Layers

New items go to the active (highlighted) layer. Lock blocks selecting and moving, and clicks pass to the layers below. Ctrl+M moves the selection to the active layer.

### Find and export

- Ctrl+F searches names of zones, links and tokens, zone descriptions, texts and notes.
- Ctrl+E exports the visible canvas at screen resolution, without selection marks.
- Export full size… exports the selection or the visible area at the maps' own resolution, up to 100 megapixels.

### Projects

- `.vaxmap` is JSON with layers and items. Maps are referenced by path, relative to the project when possible, and not embedded. Copy the images with the project.
- Save package… writes the project and all its images to one `.zip` (at most 4 GB of images). Opening the `.zip` extracts it to a new folder beside it and opens the project.
- Missing images: on open, the program asks for the first one and finds the others in the same folder. Later: right click the map, Locate image….
- Recovery copy: with unsaved changes the project is copied every minute to `recovery.vaxmap` in the data folder. The next start after a crash offers to restore it.
- Open recent… lists the last 5 projects. Window, tool, colour, width, Layers panel and minimap are restored on start.
- Projects from 0.3 open with their calibration converted to a square grid. A project from a newer version opens with a warning, and Save then asks for a new name.
- `VaxMaps.exe file` (or a file dropped on the program icon) opens a `.vaxmap`, a package `.zip`, or imports map images.

### Interface language

English and Portuguese. The first start follows the system language. The Português / English button in the menu switches it.

## Performance

Techniques taken from PureRef and Atlas VTT:

- On import each map becomes a pyramid of 512 px tiles (level 0 = original size, each level half the previous), stored in a disk cache and compressed on several cores. Only the smallest level stays in memory.
- The level drawn follows the zoom. Only visible tiles are loaded, in threads, newest request first. While a tile loads, the closest coarser level in memory is shown.
- Decoded tiles share a 1 GB LRU cache across all maps.
- Tiles are lossless PNG, so at full zoom the pixels are the original ones. Each tile carries 1 px of its neighbours to avoid seams.
- Cache location: `%LOCALAPPDATA%\VaxMaps\Vax Maps\cache\tiles2`. A textured 6000 x 4000 px map used 32 MB. Pyramids of maps not opened for 90 days are deleted at start. The cache can also be deleted while the program is closed, or from the menu: Project, Clear cache (with one window of the program open).

## Portable copy

`VaxMaps-0.6.2-portable.zip` contains `VaxMaps.exe` and a `VaxMaps-data` folder. While that folder exists, settings (`VaxMaps-data\VaxMaps\Vax Maps.ini`), tile cache and recovery copy stay in it, and nothing is written to the registry or the user's folders. Delete the folder and the program uses the usual locations. The portable copy does not associate `.vaxmap` files: open them from the program or drop them on the window.

## Build

Requires Qt 6.4+ and CMake 3.16+.

Windows (MinGW or MSVC):

```
cmake -S . -B build -DCMAKE_PREFIX_PATH=C:/Qt/6.x/mingw_64
cmake --build build --config Release
```

Linux:

```
cmake -S . -B build && cmake --build build
```

Tests: `VaxMaps --selftest <folder> [--bench]`. On Linux without a display, set `QT_QPA_PLATFORM=offscreen`.

The released `.exe` is cross-built on Linux with static Qt 6.8.5 LTS and MinGW: `tools/build-windows.sh` builds Qt (host tools and Windows libraries), the program, the installer and the portable zip. The NSIS installer script is in `tools/`. GitHub Actions runs the self-test on every push.

## Changes

See [CHANGELOG.md](CHANGELOG.md).

## Licence

AGPL-3.0, see [LICENSE](LICENSE). The manual calibration flow and two formulas (cell size from two corners, circular mean of the clicks) follow Atlas VTT, which is AGPL-3.0.
