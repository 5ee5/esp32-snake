# esp32-snake

A Snake game that plays itself on an ESP32-C3 with a tiny 72x40 OLED. The snake never crashes. It fills the whole board in about 9 minutes, then shows a win screen with the time and the best time, and starts again.

## Hardware

- ESP32-C3 board with a 0.42" 72x40 SSD1306 OLED, like the common "ESP32-C3 OLED 0.42" boards
- OLED on I2C: SDA = GPIO5, SCL = GPIO6
- USB-C for power, flashing and the serial log

If your display is wired differently, change `Wire.begin(5, 6)` in `src/main.cpp`.

## Build and flash

You need [PlatformIO](https://platformio.org/).

```bash
pio run -t upload        # build and flash
pio device monitor       # serial log (in an interactive terminal)
tools/log.sh 30          # or: print 30 seconds of the log from a script
```

The log prints one line each time the snake eats, with the score and timing:

```
Food eaten! Score: 42 (slowest step 15032 us, draw 14468 us)
```

## How it works

The board is a 24x8 grid of cells, each drawn as 3x4 pixels.

**Never crashing.** The snake follows a *Hamiltonian cycle*: a loop that visits every cell exactly once. If the snake just follows it, it can never run into itself, but it has to go all the way around the board for every piece of food.

**Getting there faster.** Two things make it quicker than that:

- **A cycle that changes shape.** The cycle is built from a spanning tree over 2x2 blocks of cells, and any such tree gives a valid cycle. Each step, the game tries swapping one edge of the tree for another. It keeps the new cycle only if it reaches the food sooner *and* the snake's body still lies along it in order. That second condition keeps the snake safe.
- **Shortcuts.** The head may also jump to a neighbouring cell further ahead on the cycle, as long as it lands before the tail. Shortcuts turn off once the board is half full.

From simulating many games:

| Strategy | Average time to fill the board |
|---|---|
| Fixed cycle | ~18 min |
| Fixed cycle + shortcuts | ~10 min |
| Changing cycle + shortcuts (this) | ~9 min |

The best time is saved in flash, so it survives reboots.

## Simulator

`tools/sim.sh` runs the real game logic from `src/main.cpp` on your computer, with the display and other hardware faked. It plays many games and fails if the snake ever collides, makes an illegal move, or gets stuck. Run it after changing the AI:

```bash
tools/sim.sh            # 300 games
tools/sim.sh 50         # quicker
```

It needs `g++`. Keep game logic above `setup()` in `main.cpp`, because that's the part the simulator uses.

The simulator checks safety, not speed on the board. The ESP32-C3 is hundreds of times slower than a desktop, so check the "slowest step" numbers in the serial log too. A step has to finish well within the 120 ms move interval.
