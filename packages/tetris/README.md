# Tetris

Installable Glaze falling-block game. Build with `./build.sh out/pkg/tetris.gzp`;
install the package from Files on a card. No extra resources are needed.

Use the BE-300 hardware buttons: Left/Right move, Down soft drops, Up rotates
clockwise, and OK hard drops. Escape pauses and opens a menu: Up/Down selects
Resume, Hold piece, or New Game; OK chooses. New Game asks for confirmation
(default Cancel). Escape cancels or resumes. Hold returns to play immediately.
All game and menu actions work without touch or a dock keyboard. A dock also
supports Enter for hard drop and Tab for hold. Home saves and pauses. Reopening
a saved game requires Resume. Best score survives starting over.

Rules: seven-bag randomizer, clockwise SRS kicks, ghost, next preview and one
hold per piece. Clear lines for 100/300/500/800 times the current level; soft and
hard drops award 1 and 2 points per cell. Ten lines advance a level. Gravity
starts at 800 ms per cell and decreases by 60 ms per level to an 80 ms minimum.
Grounded pieces lock after 500 ms, allowing up to 15 movement/rotation resets.
Locking any cell in the two hidden rows, or a blocked spawn, ends the game.

Game state uses a versioned checksum-protected record in `data/state`, replaced
through a temporary file. Invalid saves start a fresh game. Save failures appear
on screen. The record is private to this package/version, not a public format.

Run `make -C host tetris-test` for deterministic rules/save checks with ASan and
UBSan. For the emulator smoke test, put only this package on a card:

```
tools/mkcfimg.sh out/tetris-cf.img 16 16 out/pkg/tetris.gzp
CF=out/tetris-cf.img tests/run.sh tests/t_tetris.txt 46
```

To check persistence across a fresh boot after the smoke run:

```
./build.sh INPUT_SCRIPT=tests/t_tetris_resume.txt out/glaze.bin
FS_FROM=out/shots/t_tetris/glaze.bin NO_BUILD=1 tests/run.sh tests/t_tetris_resume.txt 16
```

The resume run launches the installed package from the previous filesystem,
resumes the saved board, rotates and drops a piece, then opens the pause menu.
