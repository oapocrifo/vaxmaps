# Changelog

## 0.9

Projects from 0.8 open unchanged. Doors saved by 0.9 open in 0.8 as walls.

- **Grids detected on import**: each map imported without a grid gets one found in its lines, in the background, one map after the other (reading one takes about 90 MB for a moment). The toast says on how many. Menu, Project tab: **Detect grids on import** (on by default) and **Detect grids of the selection** for maps already on the canvas.
- Detection is stricter with faint lines: below twice the minimum strength it needs at least 14 lines across the map. On 40 maps without a grid (walls on textured floor) 0.7 found a grid on 22, 0.8 on 4, 0.9 on none. The 24 maps with a grid on part of them give the same results as in 0.8.
- **Loupe** while marking corners or hex centres: hold the button and the map shows 4 times closer beside the pointer. The point is taken where the button is released.
- Hexes typed in: **Top: Pointy / Flat**. 0.8 made them pointy-top only.
- The calibration panel keeps the lines of the last 3 maps it read (about 24 MB each). Before, every map calibrated in a session stayed in memory until the program closed.
- **Spell areas** (A: sphere, then cone, then line): drag from the origin to the last square reached. The squares hit are filled and counted with the map's diagonal rule: a sphere of 2 squares is 16 squares with the rule 1, 12 with the straight line, and of 3 squares 32 with 1-2-1. On hexes, hex distance. Esc clears them with the rulers.
- **Walls** (W): click corner after corner, they snap to the calibrated map's corners. **Doors** (D): drag from corner to corner. A double click opens or closes a door; an open door lets token vision through and the tokens look again. Saved with the project.
- **Fog by room**: with the fog brush, Shift + click reveals the whole room under the pointer, Shift + Alt + click covers it. The room is found in the map's picture: floor of the colour clicked, cut at its walls, at walls and closed doors drawn over the map, and at openings about a square wide. On the test dungeon (two rooms of 8 x 8 squares joined by a corridor, faint grid, stone texture) it reveals 64 of 64 squares of the room and none of the other, in 68 ms on a map of 1500 x 1000 px and 265 ms on one of 3000 x 2000 px.
- **Commands** (Ctrl+K): type part of a command's name, in either language and without accents; arrows pick, Enter runs it.
- Keys **1 to 4** switch the tabs of the open menu; the tabs show their number.
- Fixed: erasing across a wall left its pieces as ordinary strokes, so the vision went through them.
- **Code signing** in the build: `tools/build-windows.sh` signs `VaxMaps.exe`, the installer and the uninstaller when a certificate is given (a PKCS#12 file, a token through PKCS#11, or any signing command). See Code signing in the README. The 0.9 release files are unsigned.

## 0.8

Projects from 0.7 open unchanged.

- Fixed: grid calibration on maps whose squares do not cover the whole map (a painted border, a title band, squares on one side, rooms on black). Detection (F) and the fit of a measurement took the median cell of the four quarters of the map, so two quarters without squares gave a random cell: 0.7 took 70.37 px squares on the left half for 175.7 px, squares in one quarter for 246.4 px, and the fit of a measurement there came out at 77.3 px or not at all. Now the quarters whose lines show a grid are grouped by cell size, the group with the most evidence wins, and only its quarters' lines refine the cell. A grid seen in one quarter only needs twice the evidence and at least 8 lines across the quarter, in two quarters at least 6 lines across each.
- Fixed: a straight edge where the map turns black (around rooms, below a band of squares) lined up as hexes √3 times the squares, and detection picked them. A grid now needs a line in at least a fifth of the periods along each direction.
- On 7 test maps with a grid on part of them (6 with squares, 1 with hexes), detection and the fit find the cell within 0.05 % and every corner within 0.01 cell. On 24 random maps of the same kinds (squares on a half, a band, one quarter or the centre, art or black around them, faint or strong lines, cells of 40 to 150 px), the fit of a measurement finds the grid on 24 (0.7: 13) and detection on 21 (0.7: 5). The 3 missed: faint squares of 40 and 80 px in the centre only (detection gives twice the cell) and squares of 133 px in one quarter only (fewer than 8 lines). On 7 maps without a grid (walls, painted art) detection invents none.
- Local corrections are measured where each region's lines are, not at its centre: on a dungeon with squares only in the rooms, the worst error went from 0.049 to 0.009 cell.
- Calibration steps:
  - **Skip quarter** (S): a quarter without squares or hexes no longer blocks the steps.
  - Once the map's lines confirm a grid, the quarters where they show none are skipped by themselves.
  - A measurement that does not match a grid the lines already confirmed is left out and the grid stays. 0.7 dropped the confirmed grid and asked to measure again.
  - The progress shows measured (●), skipped (–) and remaining (○) quarters.
  - The panel is shorter: Detect sits beside Squares and Hexes, the cell size and the unit share one form.
- Interface:
  - The right-click menu is in four tabs: **Tools**, **Edit**, **View**, **Project**. The tallest is 402 px, the menu was about 800 px. It opens on the tab used last, also after a restart. Copy and Duplicate are now in the menu (Edit), Find moved to Edit, Opaque fog to View.
  - Map properties in two tabs, **Grid** and **Fog of war** (with Opaque fog). Token properties: **Token** and **Vision**. Each opens on the tab used last.
  - Tool hints: the whole hint the first 3 times a tool is picked, then only its name and key.

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

