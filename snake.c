

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#ifdef _WIN32
  #include <conio.h>
  #include <windows.h>
#else
  #include <termios.h>
  #include <unistd.h>
  #include <sys/select.h>
#endif

/* ---------- Settings ---------- */
#define WIDTH        30            /* number of columns in the board */
#define HEIGHT       18            /* number of rows in the board    */
#define MAX_LEN      (WIDTH * HEIGHT)
#define SCORE_FILE   "snake_highscore.txt"
#define BONUS_TIME   45            /* bonus food disappears after 45 moves */

/* ---------- Colors (ANSI escape codes) ---------- */
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define DIM     "\033[2m"
#define RED     "\033[38;5;196m"
#define GOLD    "\033[38;5;220m"
#define CYAN    "\033[38;5;51m"
#define WHITE   "\033[38;5;255m"
#define GREY    "\033[38;5;238m"
#define BORDER  "\033[38;5;39m"

/* ---------- Data types ---------- */
typedef struct { int x, y; } Point;
typedef enum { UP, DOWN, LEFT, RIGHT } Direction;

/* ---------- Game state (global variables) ---------- */
static Point     snake[MAX_LEN];   /* snake[0] is the head */
static int       length;
static Direction dir;
static Point     food;
static Point     bonus;
static int       bonusActive, bonusTimer;
static int       score, highScore, level, foodEaten;
static int       frame;            /* counts frames, used for the color animation */

#ifndef _WIN32
static struct termios oldTerm;
#endif

/* ==========================================================================
 *  PART 1: terminal tools (work on both Windows and Linux)
 * ========================================================================== */

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void terminal_restore(void) {
    printf("\033[?25h" RESET "\n");           /* show the cursor again */
    fflush(stdout);
#ifndef _WIN32
    tcsetattr(STDIN_FILENO, TCSANOW, &oldTerm);
#endif
}

static void terminal_setup(void) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(h, &mode);
    SetConsoleMode(h, mode | 0x0004);         /* turn on ANSI colors */
    SetConsoleOutputCP(65001);                /* turn on UTF-8 symbols */
#else
    struct termios t;
    tcgetattr(STDIN_FILENO, &oldTerm);
    t = oldTerm;
    t.c_lflag &= ~(ICANON | ECHO);            /* read keys instantly, no echo */
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
#endif
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);   /* draw the whole frame at once */
    printf("\033[?25l");                      /* hide the cursor */
    atexit(terminal_restore);                 /* always restore on exit */
}

/* Is there a key waiting to be read? */
static int key_available(void) {
#ifdef _WIN32
    return _kbhit();
#else
    struct timeval tv = {0, 0};
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(STDIN_FILENO, &fds);
    return select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) > 0;
#endif
}

/* Read one key. Arrow keys are converted to w/a/s/d. Returns 0 if unknown. */
static int read_key(void) {
#ifdef _WIN32
    int c = _getch();
    if (c == 0 || c == 224) {
        c = _getch();
        switch (c) {
            case 72: return 'w';
            case 80: return 's';
            case 75: return 'a';
            case 77: return 'd';
        }
        return 0;
    }
    return tolower(c);
#else
    unsigned char c, s1, s2;
    if (read(STDIN_FILENO, &c, 1) != 1) return 0;
    if (c == 27) {                            /* arrow keys send: ESC [ A/B/C/D */
        if (key_available() && read(STDIN_FILENO, &s1, 1) == 1 &&
            key_available() && read(STDIN_FILENO, &s2, 1) == 1 && s1 == '[') {
            switch (s2) {
                case 'A': return 'w';
                case 'B': return 's';
                case 'D': return 'a';
                case 'C': return 'd';
            }
        }
        return 0;
    }
    return tolower(c);
#endif
}

/* Wait until the player presses a key */
static int wait_key(void) {
    while (!key_available()) sleep_ms(20);
    return read_key();
}

/* ==========================================================================
 *  PART 2: high score (saved in a small text file)
 * ========================================================================== */

static int load_high_score(void) {
    int v = 0;
    FILE *f = fopen(SCORE_FILE, "r");
    if (f) { if (fscanf(f, "%d", &v) != 1) v = 0; fclose(f); }
    return v;
}

static void save_high_score(int v) {
    FILE *f = fopen(SCORE_FILE, "w");
    if (f) { fprintf(f, "%d\n", v); fclose(f); }
}

/* ==========================================================================
 *  PART 3: game logic
 * ========================================================================== */

/* Is this cell occupied by the snake or by food? */
static int cell_taken(int x, int y) {
    for (int i = 0; i < length; i++)
        if (snake[i].x == x && snake[i].y == y) return 1;
    if (food.x == x && food.y == y) return 1;
    if (bonusActive && bonus.x == x && bonus.y == y) return 1;
    return 0;
}

/* Put a point on a random free cell */
static Point random_free_cell(void) {
    Point p;
    do {
        p.x = rand() % WIDTH;
        p.y = rand() % HEIGHT;
    } while (cell_taken(p.x, p.y));
    return p;
}

static void new_game(void) {
    length = 4;
    for (int i = 0; i < length; i++) {        /* start in the middle, facing right */
        snake[i].x = WIDTH / 2 - i;
        snake[i].y = HEIGHT / 2;
    }
    dir = RIGHT;
    bonusActive = 0;
    bonusTimer = 0;
    score = 0;
    level = 1;
    foodEaten = 0;
    frame = 0;
    food = random_free_cell();
}

/* Milliseconds between moves: gets smaller (faster) with each level */
static int current_delay(void) {
    int d = 150 - (level - 1) * 12;
    return d < 55 ? 55 : d;
}

/* Move the snake one step. Returns 0 if the snake died. */
static int update(void) {
    Point next = snake[0];
    switch (dir) {
        case UP:    next.y--; break;
        case DOWN:  next.y++; break;
        case LEFT:  next.x--; break;
        case RIGHT: next.x++; break;
    }

    /* 1) hit a wall? */
    if (next.x < 0 || next.x >= WIDTH || next.y < 0 || next.y >= HEIGHT)
        return 0;

    /* 2) what will we eat? */
    int ateFood  = (next.x == food.x && next.y == food.y);
    int ateBonus = (bonusActive && next.x == bonus.x && next.y == bonus.y);
    int grow     = ateFood || ateBonus;

    /* 3) hit ourselves? (the tail moves away, so ignore it unless we grow) */
    int limit = grow ? length : length - 1;
    for (int i = 0; i < limit; i++)
        if (snake[i].x == next.x && snake[i].y == next.y) return 0;

    /* 4) move: every segment takes the place of the one before it */
    Point oldTail = snake[length - 1];
    for (int i = length - 1; i > 0; i--) snake[i] = snake[i - 1];
    snake[0] = next;
    if (grow) snake[length++] = oldTail;

    /* 5) scoring */
    if (ateFood) {
        score += 10;
        foodEaten++;
        level = 1 + foodEaten / 5;            /* new level every 5 foods */
        food = random_free_cell();
        if (foodEaten % 5 == 0 && !bonusActive) {   /* golden bonus appears */
            bonus = random_free_cell();
            bonusActive = 1;
            bonusTimer = BONUS_TIME;
        }
    }
    if (ateBonus) {
        score += 50;
        bonusActive = 0;
    }
    if (bonusActive && --bonusTimer <= 0) bonusActive = 0;

    if (score > highScore) highScore = score;
    return 1;
}

/* ==========================================================================
 *  PART 4: drawing
 * ========================================================================== */

static void draw_title_screen(void) {
    printf("\033[2J\033[H\n\n");
    printf(GOLD BOLD);
    printf("     ____              _        \n");
    printf("    / ___| _ __   __ _| | _____ \n");
    printf("    \\___ \\| '_ \\ / _` | |/ / _ \\\n");
    printf("     ___) | | | | (_| |   <  __/\n");
    printf("    |____/|_| |_|\\__,_|_|\\_\\___|\n");
    printf(RESET "\n");
    printf("      " CYAN "Eat the food. Grow longer. Don't crash!\n\n" RESET);
    printf("      " WHITE "Move  " GREY ":" WHITE " W A S D  or  Arrow keys\n");
    printf("      Pause " GREY ":" WHITE " P\n");
    printf("      Quit  " GREY ":" WHITE " Q\n\n" RESET);
    printf("      " RED "● " WHITE "food +10    " GOLD "★ " WHITE "golden bonus +50\n\n" RESET);
    printf("      " GOLD "Best score: %d\n\n" RESET, highScore);
    printf("      " DIM "Press any key to start..." RESET "\n");
    fflush(stdout);
}

/* Draw one frame. Uses a small grid to know what to print in each cell. */
static void draw(const char *message) {
    int grid[HEIGHT][WIDTH];                  /* 0=empty, 1=head, 2=body, 3=food, 4=bonus */
    int bodyIndex[HEIGHT][WIDTH];
    memset(grid, 0, sizeof(grid));

    for (int i = length - 1; i >= 0; i--) {
        grid[snake[i].y][snake[i].x] = (i == 0) ? 1 : 2;
        bodyIndex[snake[i].y][snake[i].x] = i;
    }
    grid[food.y][food.x] = 3;
    if (bonusActive) grid[bonus.y][bonus.x] = 4;

    printf("\033[H");                         /* move cursor to top-left (no flicker) */

    /* Score panel */
    printf("\n  " GOLD BOLD "SNAKE" RESET "   " WHITE "Score " GOLD BOLD "%-5d" RESET
           WHITE "Best " CYAN BOLD "%-5d" RESET
           WHITE "Level " RED BOLD "%-3d" RESET
           WHITE "Length " WHITE BOLD "%-3d" RESET "\033[K\n", score, highScore, level, length);

    /* Top border */
    printf("  " BORDER "╔");
    for (int x = 0; x < WIDTH; x++) printf("══");
    printf("╗" RESET "\n");

    /* Board */
    for (int y = 0; y < HEIGHT; y++) {
        printf("  " BORDER "║" RESET);
        for (int x = 0; x < WIDTH; x++) {
            switch (grid[y][x]) {
                case 0:  printf(GREY "· " RESET); break;
                case 1:  printf("\033[38;5;190m" BOLD "██" RESET); break;
                case 2: {                     /* shimmering color wave along the body */
                    int t = (bodyIndex[y][x] + frame) % 10;
                    int k = t < 5 ? t : 9 - t;
                    printf("\033[38;5;%dm██" RESET, 46 + k);
                    break;
                }
                case 3:  printf(RED BOLD "● " RESET); break;
                case 4:  printf(GOLD BOLD "★ " RESET); break;
            }
        }
        printf(BORDER "║" RESET "\n");
    }

    /* Bottom border */
    printf("  " BORDER "╚");
    for (int x = 0; x < WIDTH; x++) printf("══");
    printf("╝" RESET "\n");

    /* Status line (bonus timer bar or message) */
    if (message) {
        printf("  %s\033[K\n", message);
    } else if (bonusActive) {
        int bars = bonusTimer * 20 / BONUS_TIME;
        printf("  " GOLD "★ BONUS " RESET GOLD);
        for (int i = 0; i < 20; i++) printf(i < bars ? "█" : "░");
        printf(RESET "\033[K\n");
    } else {
        printf("  " DIM "W A S D / Arrows: move   P: pause   Q: quit" RESET "\033[K\n");
    }
    fflush(stdout);
}

static void draw_game_over(int newRecord) {
    draw(NULL);
    printf("\n  " RED BOLD "╔══════════════════════════════════╗\n");
    printf("  ║            GAME  OVER            ║\n");
    printf("  ╠══════════════════════════════════╣\n" RESET);
    printf("  " RED BOLD "║" RESET WHITE "   Final score : " GOLD BOLD "%-16d" RESET RED BOLD "║\n", score);
    printf("  " RED BOLD "║" RESET WHITE "   Level reached: " CYAN BOLD "%-15d" RESET RED BOLD "║\n", level);
    if (newRecord)
        printf("  " RED BOLD "║" RESET GOLD BOLD "   *** NEW HIGH SCORE! ***        " RESET RED BOLD "║\n");
    printf("  ╚══════════════════════════════════╝\n" RESET);
    printf("\n  " WHITE "Press " GOLD "R" WHITE " to play again or " GOLD "Q" WHITE " to quit.\n" RESET);
    fflush(stdout);
}

/* ==========================================================================
 *  PART 5: the main loop
 * ========================================================================== */

/* Play one full game. Returns 1 if the player wants to quit the program. */
static int play(void) {
    int previousBest = highScore;
    new_game();
    printf("\033[2J");                        /* clear the screen once */

    while (1) {
        draw(NULL);
        frame++;

        /* Wait for the delay, but keep listening to the keyboard */
        int waited = 0, delay = current_delay();
        Direction wanted = dir;
        while (waited < delay) {
            while (key_available()) {
                int k = read_key();
                if      (k == 'w' && dir != DOWN)  wanted = UP;
                else if (k == 's' && dir != UP)    wanted = DOWN;
                else if (k == 'a' && dir != RIGHT) wanted = LEFT;
                else if (k == 'd' && dir != LEFT)  wanted = RIGHT;
                else if (k == 'q') return 1;
                else if (k == 'p') {
                    draw("\033[1m\033[38;5;220m  ⏸  PAUSED - press P to continue\033[0m");
                    while (wait_key() != 'p') { }
                }
            }
            sleep_ms(10);
            waited += 10;
        }
        dir = wanted;

        if (!update()) break;
    }

    int newRecord = (score > previousBest);
    if (newRecord) save_high_score(highScore);
    draw_game_over(newRecord);

    while (1) {
        int k = wait_key();
        if (k == 'r') return 0;
        if (k == 'q') return 1;
    }
}

int main(void) {
    srand((unsigned) time(NULL));
    terminal_setup();
    highScore = load_high_score();

    draw_title_screen();
    wait_key();

    while (!play()) { }                       /* keep playing until the player quits */

    printf("\033[2J\033[H" GOLD "Thanks for playing! Best score: %d\n" RESET, highScore);
    return 0;
}