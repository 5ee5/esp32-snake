# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

PlatformIO / Arduino-framework firmware for an ESP32-C3 board (`esp32-c3-devkitm-1`) with a 72x40 SSD1306 OLED. It runs a self-playing Snake game. The only dependency is `olikraus/U8g2`. There are no unit tests, but `tools/sim.sh` runs the game AI on the host as a safety check.

## Commands

```bash
pio run                      # build
pio run -t upload            # build and flash
pio device monitor           # serial monitor (115200 baud, set in platformio.ini)
pio run -t upload -t monitor # flash, then open the monitor
pio run -t clean
tools/sim.sh [games]         # simulate games on the host (needs g++), fails on any collision
tools/log.sh [seconds]       # print the serial log without an interactive terminal
```

## Hardware assumptions (src/main.cpp)

- OLED driver: `U8G2_SSD1306_72X40_ER_F_HW_I2C` (full-frame buffer, hardware I2C).
- I2C pins: SDA=5, SCL=6 (`Wire.begin(5, 6)`), bus at 400 kHz (`setBusClock`). A full frame takes about 13–15 ms to send.
- Serial goes over the native USB port (`ARDUINO_USB_CDC_ON_BOOT=1` in `platformio.ini`), shown as `/dev/ttyACM0`. `setTxTimeoutMs(0)` stops prints from stalling the game when no computer is reading. The chip can't tell when the port is opened, and lines printed while nobody is reading stay in its USB buffers. They arrive in one burst on connect. `monitor/filter_fresh.py` (enabled via `monitor_filters`) drops whatever arrives in the first 0.5 s. `pio device monitor` needs an interactive terminal. Use `tools/log.sh [seconds]` from scripts or Claude Code; it does the same dropping.
- Screen layout: an 8px header row with the title and score, then a 72x32 play area. The play area is a 24x8 grid of 3x4-pixel cells. The header moves right by `gameCount % 4` pixels each game so it doesn't burn in. The panel is monochrome, so colours aren't possible.

## Game architecture

The snake is guided by a **Hamiltonian cycle**, so it can never collide with itself. The cycle is reshaped while the game runs, and the snake takes shortcuts across it, so it reaches food faster than going all the way around.

- Cells are numbered `y * GRID_W + x`. `body[]` is a ring buffer of cells (tail at `tailSlot`, `snakeLength` entries), and `occupied[]`, `headPos` and `foodPos` are cells. `cycleOrder[cell]` gives the cell's position along the current cycle.
- The cycle comes from a **spanning tree over 2x2 blocks** (`inTree[]`, 12x4 blocks; horizontal edges come first, then vertical, see `hEdge`/`vEdge`). `nextCell()` walks around the outside of the tree: each block loops TL→BL→BR→TR on its own, and each tree edge joins two neighbouring loops. Any spanning tree gives a valid cycle, so both `GRID_W` and `GRID_H` must be even (`static_assert`). Each game starts from a comb tree (`buildStartTree()`).
- **Safety invariant:** walking the cycle from the tail visits the body cells in order and reaches the head last (`followsBody()`). As long as this holds, following the cycle is always safe.
- Each step, `improveCycle()` tries tree edge swaps (add a non-tree edge, remove an edge from the loop it forms), up to `MAX_SWAPS_PER_STEP`. It keeps a swap only if the invariant still holds and the food gets closer along the cycle. Then `chooseNextMove()` applies Tapsell's shortcut rule: jump to a neighbour further ahead on the cycle if it lands before the tail (with a margin) and not past the food. Shortcuts are turned off once the board is half full.
- If you change the AI, re-check it with `tools/sim.sh`, which plays many games on the host and fails on any collision, illegal move or stuck game. A bug here is easy to introduce and hard to notice on the device.
- **CPU budget:** the ESP32-C3 is hundreds of times slower than a desktop, so host timings from `tools/sim.sh` don't predict device timings. The serial log prints the slowest step since the last food, so check it on the board after changing `improveCycle()`. To stay fast, each candidate swap first walks from the head only as far as the current best distance (`walkDistance()`), and only swaps that pass get the full `buildOrder()` + `followsBody()` check. `nextCell()` is table-driven (`buildNextTables()`). In simulation (300 games) it averages about 4,600 steps per full board, versus about 9,100 for a fixed cycle and about 5,150 for shortcuts alone. More swaps per step, sideways swaps and steering the tree along a planned block route did not help. The limit is the invariant itself: food that spawns in a gap behind the head still costs nearly a full lap.
- `loop()` is non-blocking and timed with `millis()`. Each step (`MOVE_INTERVAL` ms) calls `stepGame()`, which reshapes the cycle, moves the head, grows the snake or advances the tail, and redraws the whole frame (`renderGame()`). `drawSegment()` draws each body cell 1px smaller than its cell and fills the gap only towards the next segment, so the body's path stays visible. Food is drawn as a hollow cell. `lastMove += MOVE_INTERVAL` keeps a steady pace.
- When the snake fills the grid, `onWin()` saves the best time (fewest steps) to NVS via `Preferences` (namespace `snake`, key `best`). `loop()` then shows the full board for 1 s, then `renderWin()` (time and best) until `WIN_SCREEN_TIME`, then calls `resetGame()`.
- The displayed score is `snakeLength - START_LENGTH`.
- `tools/sim.sh` extracts everything in `main.cpp` above `setup()` and stubs the hardware APIs. Keep game logic above `setup()`, keep it free of other Arduino calls, and keep `stepGame()` as the single-step entry point. Otherwise the simulator breaks.
