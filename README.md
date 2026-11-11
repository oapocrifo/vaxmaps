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

## Licence note

The manual calibration flow and two small formulas (cell size from two corners, circular mean of the clicks) follow Atlas VTT, which is AGPL-3.0. If you distribute Vax Maps, publish it under AGPL-3.0.
