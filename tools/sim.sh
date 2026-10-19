#!/bin/bash
# Run the Snake AI from src/main.cpp on the host for many games and
# check that it never collides, makes an illegal move or gets stuck.
#
# usage: tools/sim.sh [games] [main.cpp]
#
# The game logic is taken from main.cpp up to setup(), with the Arduino,
# display and Preferences APIs stubbed out. stepGame() is the real code.
set -euo pipefail

GAMES=${1:-300}
SRC=${2:-"$(dirname "$0")/../src/main.cpp"}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

{
cat <<'EOF'
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

// Stubs for the hardware APIs main.cpp uses.
struct P {
    void begin(const char *, bool) {}
    void putULong(const char *, unsigned long) {}
    unsigned long getULong(const char *, unsigned long d) { return d; }
};
struct D {
    template <class... A> void printf(const char *, A...) {}
    void print(...) {}
    void setCursor(int, int) {}
    void drawStr(int, int, const char *) {}
    void drawBox(int, int, int, int) {}
    void drawFrame(int, int, int, int) {}
    void clearBuffer() {}
    void sendBuffer() {}
    void setFont(int) {}
    void begin() {}
};
struct S_ {
    template <class... A> void printf(const char *, A...) {}
    void println(const char *) {}
    void begin(int) {}
} Serial;
struct W { void begin(int, int) {} } Wire;
#define u8g2_font_5x7_tf 0
long random(long n) { return rand() % n; }
unsigned long millis() { return 0; }
EOF

sed -e '/^#include/d' \
    -e '/^U8G2_SSD1306/,/^);/d' \
    -e 's/^Preferences prefs;/P prefs; D display;/' \
    -e '/^void setup/,$d' \
    "$SRC"

cat <<'EOF'
int main(int argc, char **argv) {
    int games = atoi(argv[1]);
    long total = 0, worst = 0, best = 1e9;
    clock_t start = clock();

    for (int g = 0; g < games; g++) {
        srand(g);
        resetGame();

        while (!won) {
            int prevHead = headPos;
            stepGame();

            int moved = abs(cellX(headPos) - cellX(prevHead)) +
                        abs(cellY(headPos) - cellY(prevHead));
            if (moved != 1) {
                printf("FAIL: non-adjacent move, game %d step %lu\n", g, steps);
                return 1;
            }

            static bool seen[CELL_COUNT];
            memset(seen, 0, sizeof(seen));
            for (int i = 0; i < snakeLength; i++) {
                int cell = body[(tailSlot + i) % CELL_COUNT];
                if (seen[cell]) {
                    printf("FAIL: collision, game %d step %lu length %d\n",
                           g, steps, snakeLength);
                    return 1;
                }
                seen[cell] = true;
            }
            for (int c = 0; c < CELL_COUNT; c++) {
                if (seen[c] != occupied[c]) {
                    printf("FAIL: occupied[] out of sync, game %d step %lu\n", g, steps);
                    return 1;
                }
            }

            if (steps > 200000) {
                printf("FAIL: stuck, game %d\n", g);
                return 1;
            }
        }

        total += steps;
        if ((long)steps > worst) worst = steps;
        if ((long)steps < best) best = steps;
    }

    double ms = (clock() - start) * 1000.0 / CLOCKS_PER_SEC;
    printf("OK: %d games, avg %ld steps (%.1f min), best %ld, worst %ld, "
           "host %.3f ms/step\n",
           games, total / games, total / games * MOVE_INTERVAL / 60000.0,
           best, worst, ms / total);
}
EOF
} > "$WORK/sim.cpp"

g++ -O2 -std=c++17 -w "$WORK/sim.cpp" -o "$WORK/sim"
"$WORK/sim" "$GAMES"
