
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_system.h>

U8G2_SSD1306_72X40_ER_F_HW_I2C display(
    U8G2_R0,
    U8X8_PIN_NONE
);

Preferences prefs;

// OLED: 72x40 pixels.
// Header: 8 pixels. Game area: 72x32.
constexpr int CELL_W = 3;
constexpr int CELL_H = 4;
constexpr int GRID_W = 24;
constexpr int GRID_H = 8;
constexpr int CELL_COUNT = GRID_W * GRID_H;
constexpr int START_LENGTH = 3;

// The cycle is built from 2x2 blocks of cells. Any spanning tree over
// the blocks gives a Hamiltonian cycle that runs around the tree.
static_assert(GRID_W % 2 == 0 && GRID_H % 2 == 0, "grid must split into 2x2 blocks");

constexpr int BLOCK_W = GRID_W / 2;
constexpr int BLOCK_H = GRID_H / 2;
constexpr int BLOCK_COUNT = BLOCK_W * BLOCK_H;
constexpr int H_EDGES = (BLOCK_W - 1) * BLOCK_H;
constexpr int EDGE_COUNT = H_EDGES + BLOCK_W * (BLOCK_H - 1);

// How many tree edge swaps to try per step when looking for a
// cycle that reaches the food sooner.
constexpr int MAX_SWAPS_PER_STEP = 1;

// Cells on the edge of the grid use NO_EDGE, which is never in the tree.
constexpr int NO_EDGE = EDGE_COUNT;

bool inTree[EDGE_COUNT + 1];

// Cell -> position along the current cycle.
int cycleOrder[CELL_COUNT];
int candidateOrder[CELL_COUNT];

// Cells are numbered y * GRID_W + x.
// The body is a ring buffer of cells, from tail to head.
int body[CELL_COUNT];
bool occupied[CELL_COUNT];
int tailSlot = 0;
int snakeLength = START_LENGTH;
int headPos = 0;
int foodPos = -1;

unsigned long lastMove = 0;
unsigned long slowestStep = 0;
constexpr unsigned long MOVE_INTERVAL = 120;

unsigned long steps = 0;
unsigned long gameCount = 0;
unsigned long bestSteps = 0;

bool won = false;
unsigned long wonAt = 0;
constexpr unsigned long WIN_SCREEN_TIME = 5000;

int cellX(int cell) {
    return cell % GRID_W;
}

int cellY(int cell) {
    return cell / GRID_W;
}

int hEdge(int bx, int by) {
    return by * (BLOCK_W - 1) + bx;
}

int vEdge(int bx, int by) {
    return H_EDGES + by * BLOCK_W + bx;
}

void edgeBlocks(int edge, int &a, int &b) {
    if (edge < H_EDGES) {
        int by = edge / (BLOCK_W - 1);
        int bx = edge % (BLOCK_W - 1);

        a = by * BLOCK_W + bx;
        b = a + 1;
    } else {
        a = edge - H_EDGES;
        b = a + BLOCK_W;
    }
}

// Each block on its own is a small loop:
// top-left -> bottom-left -> bottom-right -> top-right -> top-left.
// A tree edge between two blocks opens both loops and joins them.
//
// Each cell's successor depends on at most one tree edge, so it is
// looked up from tables built once.
int nextIfApart[CELL_COUNT];
int nextIfJoined[CELL_COUNT];
int joinEdge[CELL_COUNT];

void buildNextTables() {
    for (int cell = 0; cell < CELL_COUNT; cell++) {
        int x = cellX(cell);
        int y = cellY(cell);
        int bx = x / 2;
        int by = y / 2;
        bool right = x % 2;
        bool bottom = y % 2;

        int edge = NO_EDGE;

        if (!right && !bottom) {
            nextIfApart[cell] = cell + GRID_W;
            nextIfJoined[cell] = cell - 1;
            if (bx > 0) edge = hEdge(bx - 1, by);
        } else if (!right && bottom) {
            nextIfApart[cell] = cell + 1;
            nextIfJoined[cell] = cell + GRID_W;
            if (by < BLOCK_H - 1) edge = vEdge(bx, by);
        } else if (right && bottom) {
            nextIfApart[cell] = cell - GRID_W;
            nextIfJoined[cell] = cell + 1;
            if (bx < BLOCK_W - 1) edge = hEdge(bx, by);
        } else {
            nextIfApart[cell] = cell - 1;
            nextIfJoined[cell] = cell - GRID_W;
            if (by > 0) edge = vEdge(bx, by - 1);
        }

        joinEdge[cell] = edge;
    }
}

int nextCell(int cell) {
    return inTree[joinEdge[cell]] ? nextIfJoined[cell] : nextIfApart[cell];
}

void buildOrder(int order[]) {
    int cell = 0;

    for (int i = 0; i < CELL_COUNT; i++) {
        order[cell] = i;
        cell = nextCell(cell);
    }
}

// Start from a comb: across the top row of blocks, then straight down
// every block column.
void buildStartTree() {
    buildNextTables();
    memset(inTree, 0, sizeof(inTree));

    for (int bx = 0; bx < BLOCK_W - 1; bx++) {
        inTree[hEdge(bx, 0)] = true;
    }

    for (int bx = 0; bx < BLOCK_W; bx++) {
        for (int by = 0; by < BLOCK_H - 1; by++) {
            inTree[vEdge(bx, by)] = true;
        }
    }

    buildOrder(cycleOrder);
}

// Find the tree edges on the path between two blocks.
int treePath(int from, int to, int pathEdges[]) {
    int parentEdge[BLOCK_COUNT];
    int queue[BLOCK_COUNT];
    int head = 0;
    int tail = 0;

    for (int i = 0; i < BLOCK_COUNT; i++) {
        parentEdge[i] = -1;
    }

    queue[tail++] = from;
    parentEdge[from] = EDGE_COUNT;

    while (head < tail) {
        int block = queue[head++];

        if (block == to) {
            break;
        }

        int bx = block % BLOCK_W;
        int by = block / BLOCK_W;

        int edges[4] = {
            bx > 0 ? hEdge(bx - 1, by) : -1,
            bx < BLOCK_W - 1 ? hEdge(bx, by) : -1,
            by > 0 ? vEdge(bx, by - 1) : -1,
            by < BLOCK_H - 1 ? vEdge(bx, by) : -1,
        };
        int neighbours[4] = {
            block - 1, block + 1, block - BLOCK_W, block + BLOCK_W
        };

        for (int d = 0; d < 4; d++) {
            if (edges[d] < 0 || !inTree[edges[d]]) {
                continue;
            }

            if (parentEdge[neighbours[d]] == -1) {
                parentEdge[neighbours[d]] = edges[d];
                queue[tail++] = neighbours[d];
            }
        }
    }

    int count = 0;
    int block = to;

    while (block != from) {
        int edge = parentEdge[block];
        int a, b;

        edgeBlocks(edge, a, b);
        pathEdges[count++] = edge;
        block = (a == block) ? b : a;
    }

    return count;
}

// How many steps forward along the cycle it takes to get from one
// position to another.
int pathDistance(int from, int to) {
    int distance = to - from;

    return distance < 0 ? distance + CELL_COUNT : distance;
}

// Steps along the cycle from one cell to another, giving up once it
// reaches the limit.
int walkDistance(int from, int to, int limit) {
    int cell = from;

    for (int distance = 1; distance < limit; distance++) {
        cell = nextCell(cell);

        if (cell == to) {
            return distance;
        }
    }

    return limit;
}

// A cycle is safe to follow if walking it from the tail passes the
// body cells in order and reaches the head last.
bool followsBody(const int order[]) {
    int tailOrder = order[body[tailSlot]];
    int previous = 0;
    int slot = tailSlot;

    for (int i = 1; i < snakeLength; i++) {
        if (++slot == CELL_COUNT) {
            slot = 0;
        }

        int distance = pathDistance(tailOrder, order[body[slot]]);

        if (distance <= previous) {
            return false;
        }

        previous = distance;
    }

    return true;
}

// Reshape the cycle so it reaches the food sooner. Each swap adds one
// edge to the block tree and removes another from the loop that forms,
// which keeps it a spanning tree. Only cycles that the body still
// follows are kept, so the snake stays safe.
void improveCycle() {
    int bestDistance =
        pathDistance(cycleOrder[headPos], cycleOrder[foodPos]);

    for (int swap = 0; swap < MAX_SWAPS_PER_STEP; swap++) {
        if (bestDistance <= 1) {
            break;
        }

        int bestAdd = -1;
        int bestRemove = -1;

        for (int add = 0; add < EDGE_COUNT; add++) {
            if (inTree[add]) {
                continue;
            }

            int a, b;
            int pathEdges[BLOCK_COUNT];

            edgeBlocks(add, a, b);
            int count = treePath(a, b, pathEdges);

            inTree[add] = true;

            for (int i = 0; i < count; i++) {
                int remove = pathEdges[i];

                inTree[remove] = false;
                // Most swaps don't bring the food closer, so check that
                // first. The full safety check is much more expensive.
                int distance = walkDistance(headPos, foodPos, bestDistance);

                if (distance < bestDistance) {
                    buildOrder(candidateOrder);

                    if (followsBody(candidateOrder)) {
                        bestDistance = distance;
                        bestAdd = add;
                        bestRemove = remove;
                    }
                }

                inTree[remove] = true;
            }

            inTree[add] = false;
        }

        if (bestAdd < 0) {
            break;
        }

        inTree[bestAdd] = true;
        inTree[bestRemove] = false;
    }

    buildOrder(cycleOrder);
}

bool isOccupied(int candidate) {
    return occupied[candidate];
}

void spawnFood() {
    if (snakeLength >= CELL_COUNT) {
        won = true;
        foodPos = -1;
        return;
    }

    do {
        foodPos = random(CELL_COUNT);
    } while (isOccupied(foodPos));
}

void resetGame() {
    buildStartTree();
    memset(occupied, 0, sizeof(occupied));

    tailSlot = 0;
    snakeLength = START_LENGTH;

    int cell = 0;

    for (int i = 0; i < snakeLength; i++) {
        body[i] = cell;
        occupied[cell] = true;
        headPos = cell;
        cell = nextCell(cell);
    }

    steps = 0;
    gameCount++;
    won = false;

    spawnFood();
}

// Pick the next cell for the head.
//
// Following the cycle is always safe. To go faster, the snake may jump
// to a neighbouring cell further ahead on the cycle, as long as it lands
// before the tail. That keeps the body ordered along the cycle, so the
// cycle still leads the head safely around to the tail.
int chooseNextMove() {
    int headOrder = cycleOrder[headPos];
    int toFood = pathDistance(headOrder, cycleOrder[foodPos]);
    int toTail = pathDistance(headOrder, cycleOrder[body[tailSlot]]);
    int emptyCells = CELL_COUNT - snakeLength - 1;

    // Leave room to grow by one cell, plus a safety margin.
    int maxSkip = toTail - 4;

    if (emptyCells < CELL_COUNT / 2) {
        // Once the board is half full, just follow the cycle.
        maxSkip = 0;
    } else if (toFood < toTail) {
        maxSkip -= 1;

        // The food sits far from the tail. Be careful not to cut off
        // the space the snake will need after eating it.
        if ((toTail - toFood) * 4 > emptyCells) {
            maxSkip -= 10;
        }
    }

    // Never jump past the food.
    if (maxSkip > toFood) {
        maxSkip = toFood;
    }

    int best = nextCell(headPos);
    int bestDistance = 1;

    const int dx[] = {1, -1, 0, 0};
    const int dy[] = {0, 0, 1, -1};

    for (int d = 0; d < 4; d++) {
        int x = cellX(headPos) + dx[d];
        int y = cellY(headPos) + dy[d];

        if (x < 0 || x >= GRID_W || y < 0 || y >= GRID_H) {
            continue;
        }

        int candidate = y * GRID_W + x;

        if (isOccupied(candidate)) {
            continue;
        }

        int distance = pathDistance(headOrder, cycleOrder[candidate]);

        if (distance > bestDistance && distance <= maxSkip) {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
}

// Advance the game by one move. Returns true if the snake ate.
bool stepGame() {
    steps++;

    improveCycle();
    int next = chooseNextMove();
    bool ate = next == foodPos;

    // Grow, or move the tail along.
    if (ate) {
        snakeLength++;
    } else {
        occupied[body[tailSlot]] = false;
        tailSlot = (tailSlot + 1) % CELL_COUNT;
    }

    body[(tailSlot + snakeLength - 1) % CELL_COUNT] = next;
    occupied[next] = true;
    headPos = next;

    if (ate) {
        spawnFood();
    }

    return ate;
}

void printTime(unsigned long stepCount) {
    unsigned long seconds = stepCount * MOVE_INTERVAL / 1000;

    display.printf("%lu:%02lu", seconds / 60, seconds % 60);
}

// Each segment is drawn one pixel smaller than its cell, leaving a gap
// on its right and bottom edges. The gap is filled only towards the
// next segment, so the body's path stays visible where it folds back
// alongside itself.
void drawSegment(int cell, int next) {
    int x = cellX(cell) * CELL_W;
    int y = 8 + cellY(cell) * CELL_H;

    display.drawBox(x, y, CELL_W - 1, CELL_H - 1);

    if (next < 0) {
        return;
    }

    int left = cellX(cell) < cellX(next) ? cell : next;
    int top = cellY(cell) < cellY(next) ? cell : next;

    if (cellY(cell) == cellY(next)) {
        display.drawBox(cellX(left) * CELL_W + CELL_W - 1, y, 1, CELL_H - 1);
    } else {
        display.drawBox(x, 8 + cellY(top) * CELL_H + CELL_H - 1, CELL_W - 1, 1);
    }
}

void renderGame() {
    display.clearBuffer();
    display.setFont(u8g2_font_5x7_tf);

    // Shift the header a little each game so its pixels don't burn in.
    int shift = gameCount % 4;

    display.drawStr(shift, 7, "SNAKE");

    display.setCursor(40 + shift, 7);
    display.print("S:");
    display.print(snakeLength - START_LENGTH);

    // Draw food as a hollow cell so it stands out from the body.
    if (foodPos >= 0) {
        int x = cellX(foodPos) * CELL_W;
        int y = 8 + cellY(foodPos) * CELL_H;

        display.drawFrame(x, y, CELL_W, CELL_H);
    }

    for (int i = 0; i < snakeLength; i++) {
        int cell = body[(tailSlot + i) % CELL_COUNT];
        int next = i + 1 < snakeLength
            ? body[(tailSlot + i + 1) % CELL_COUNT]
            : -1;

        drawSegment(cell, next);
    }

    display.sendBuffer();
}

void renderWin() {
    display.clearBuffer();
    display.setFont(u8g2_font_5x7_tf);

    display.drawStr(16, 9, "YOU WIN!");

    display.setCursor(6, 22);
    display.print("TIME ");
    printTime(steps);

    display.setCursor(6, 34);
    display.print("BEST ");
    printTime(bestSteps);

    display.sendBuffer();
}

void onWin() {
    wonAt = millis();

    bool newBest = bestSteps == 0 || steps < bestSteps;

    if (newBest) {
        bestSteps = steps;
        prefs.putULong("best", bestSteps);
    }

    Serial.printf(
        "Board filled in %lu steps%s\n",
        steps,
        newBest ? " (new best!)" : ""
    );
}

void setup() {
    Serial.begin(115200);

    // Don't stall the game waiting for USB when no computer is reading.
    Serial.setTxTimeoutMs(0);

    Wire.begin(5, 6);
    display.setBusClock(400000);
    display.begin();

    randomSeed(esp_random());

    prefs.begin("snake", false);
    bestSteps = prefs.getULong("best", 0);

    resetGame();
    renderGame();
    lastMove = millis();

    Serial.println("Autonomous Snake started!");
    Serial.println("AI: dynamic Hamiltonian cycle with shortcuts");
    Serial.println("OLED: 72x40");
}

void loop() {
    unsigned long now = millis();

    if (won) {
        // Show the full board for a moment, then the results,
        // then start over.
        if (now - wonAt >= WIN_SCREEN_TIME) {
            resetGame();
            lastMove = now;
            renderGame();
        } else if (now - wonAt >= 1000) {
            renderWin();
        }

        return;
    }

    if (now - lastMove < MOVE_INTERVAL) {
        return;
    }

    // Keep a steady pace even when a step runs a little late.
    lastMove += MOVE_INTERVAL;

    unsigned long stepStart = micros();
    bool ate = stepGame();
    unsigned long stepTime = micros() - stepStart;

    renderGame();
    unsigned long frameTime = micros() - stepStart - stepTime;

    if (stepTime > slowestStep) {
        slowestStep = stepTime;
    }

    if (ate) {
        Serial.printf(
            "Food eaten! Score: %d (slowest step %lu us, draw %lu us)\n",
            snakeLength - START_LENGTH,
            slowestStep,
            frameTime
        );

        slowestStep = 0;

        if (won) {
            onWin();
        }
    }
}
