#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <gui.h>

EYN_CMDMETA_V1("Classic snake game with keyboard controls.", "snake");

#define SNAKE_GRID_W 24
#define SNAKE_GRID_H 18
#define SNAKE_CELL 12
#define SNAKE_MAX_LEN (SNAKE_GRID_W * SNAKE_GRID_H)

typedef struct {
    int score;
    int paused;
    int dead;
    int dir;
    int next_dir;
    int length;
    int food_x;
    int food_y;
    int frame_counter;
    uint32_t rng;
    int x[SNAKE_MAX_LEN];
    int y[SNAKE_MAX_LEN];
} snake_state_t;

static uint32_t rng_next(snake_state_t* s) {
    s->rng = s->rng * 1103515245u + 12345u;
    return s->rng;
}

static int snake_contains(const snake_state_t* s, int gx, int gy) {
    for (int i = 0; i < s->length; ++i) {
        if (s->x[i] == gx && s->y[i] == gy) return 1;
    }
    return 0;
}

static void spawn_food(snake_state_t* s) {
    int attempts = 0;
    do {
        s->food_x = (int)(rng_next(s) % SNAKE_GRID_W);
        s->food_y = (int)(rng_next(s) % SNAKE_GRID_H);
        ++attempts;
        if (attempts > 1024) break;
    } while (snake_contains(s, s->food_x, s->food_y));
}

static void reset_snake(snake_state_t* s) {
    memset(s, 0, sizeof(*s));
    s->rng = 0xA18E1234u;
    s->dir = 1;
    s->next_dir = 1;
    s->length = 4;

    int sx = SNAKE_GRID_W / 2;
    int sy = SNAKE_GRID_H / 2;
    for (int i = 0; i < s->length; ++i) {
        s->x[i] = sx - i;
        s->y[i] = sy;
    }

    spawn_food(s);
}

static int is_opposite_dir(int a, int b) {
    return (a == 0 && b == 2) || (a == 2 && b == 0) || (a == 1 && b == 3) || (a == 3 && b == 1);
}

static int tick_interval(const snake_state_t* s) {
    int interval = 10 - (s->score / 50);
    if (interval < 4) interval = 4;
    return interval;
}

static void step_snake(snake_state_t* s) {
    if (s->dead || s->paused) return;

    if (!is_opposite_dir(s->dir, s->next_dir)) {
        s->dir = s->next_dir;
    }

    int dx = 0;
    int dy = 0;
    if (s->dir == 0) dy = -1;
    if (s->dir == 1) dx = 1;
    if (s->dir == 2) dy = 1;
    if (s->dir == 3) dx = -1;

    int nx = s->x[0] + dx;
    int ny = s->y[0] + dy;

    if (nx < 0 || nx >= SNAKE_GRID_W || ny < 0 || ny >= SNAKE_GRID_H) {
        s->dead = 1;
        return;
    }

    int tail_x = s->x[s->length - 1];
    int tail_y = s->y[s->length - 1];

    for (int i = s->length - 1; i > 0; --i) {
        s->x[i] = s->x[i - 1];
        s->y[i] = s->y[i - 1];
    }
    s->x[0] = nx;
    s->y[0] = ny;

    for (int i = 1; i < s->length; ++i) {
        if (s->x[i] == nx && s->y[i] == ny) {
            s->dead = 1;
            return;
        }
    }

    if (nx == s->food_x && ny == s->food_y) {
        if (s->length < SNAKE_MAX_LEN) {
            s->x[s->length] = tail_x;
            s->y[s->length] = tail_y;
            ++s->length;
        }
        s->score += 10;
        spawn_food(s);
    }
}

static void draw_snake(int h, const snake_state_t* s) {
    gui_size_t sz = {0, 0};
    (void)gui_get_content_size(h, &sz);
    if (sz.w <= 0) sz.w = 460;
    if (sz.h <= 0) sz.h = 300;

    int grid_w_px = SNAKE_GRID_W * SNAKE_CELL;
    int grid_h_px = SNAKE_GRID_H * SNAKE_CELL;
    int ox = 14;
    int oy = 26;

    (void)gui_begin(h);

    gui_rgb_t bg = {GUI_PAL_BG_R, GUI_PAL_BG_G, GUI_PAL_BG_B, 0};
    (void)gui_clear(h, &bg);

    gui_rect_t frame = {
        .x = ox - 2,
        .y = oy - 2,
        .w = grid_w_px + 4,
        .h = grid_h_px + 4,
        .r = GUI_PAL_HEADER_R,
        .g = GUI_PAL_HEADER_G,
        .b = GUI_PAL_HEADER_B,
        ._pad = 0
    };
    (void)gui_fill_rect(h, &frame);

    gui_rect_t grid = {
        .x = ox,
        .y = oy,
        .w = grid_w_px,
        .h = grid_h_px,
        .r = 26,
        .g = 28,
        .b = 30,
        ._pad = 0
    };
    (void)gui_fill_rect(h, &grid);

    gui_rect_t food = {
        .x = ox + s->food_x * SNAKE_CELL,
        .y = oy + s->food_y * SNAKE_CELL,
        .w = SNAKE_CELL - 1,
        .h = SNAKE_CELL - 1,
        .r = 240,
        .g = 100,
        .b = 100,
        ._pad = 0
    };
    (void)gui_fill_rect(h, &food);

    for (int i = s->length - 1; i >= 0; --i) {
        gui_rect_t part = {
            .x = ox + s->x[i] * SNAKE_CELL,
            .y = oy + s->y[i] * SNAKE_CELL,
            .w = SNAKE_CELL - 1,
            .h = SNAKE_CELL - 1,
            .r = (uint8_t)(i == 0 ? 170 : 120),
            .g = (uint8_t)(i == 0 ? 250 : 200),
            .b = (uint8_t)(i == 0 ? 170 : 120),
            ._pad = 0
        };
        (void)gui_fill_rect(h, &part);
    }

    char title[96];
    snprintf(title, sizeof(title), "Snake  Score:%d  Length:%d", s->score, s->length);
    gui_text_t t0 = {14, 8, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, 0, title};
    gui_text_t t1 = {14, sz.h - 16, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B, 0, "Arrows move  P pause  R restart  Q quit"};
    (void)gui_draw_text(h, &t0);
    (void)gui_draw_text(h, &t1);

    if (s->paused) {
        gui_text_t msg = {ox + grid_w_px + 18, oy + 12, 255, 236, 145, 0, "Paused"};
        (void)gui_draw_text(h, &msg);
    }
    if (s->dead) {
        gui_text_t msg1 = {ox + grid_w_px + 18, oy + 12, 255, 130, 130, 0, "Game Over"};
        gui_text_t msg2 = {ox + grid_w_px + 18, oy + 24, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, 0, "Press R to restart"};
        (void)gui_draw_text(h, &msg1);
        (void)gui_draw_text(h, &msg2);
    }

    (void)gui_present(h);
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: snake");
        return 0;
    }

    int h = gui_create("Snake", "Arrows move, q quits");
    if (h < 0) h = gui_attach("Snake", "Arrows move, q quits");
    if (h < 0) {
        puts("snake: failed to initialize GUI");
        return 1;
    }

    (void)gui_set_continuous_redraw(h, 1);

    snake_state_t st;
    reset_snake(&st);

    int running = 1;
    while (running) {
        gui_event_t ev;
        while (gui_poll_event(h, &ev) > 0) {
            if (ev.type == GUI_EVENT_CLOSE) {
                running = 0;
                break;
            }
            if (ev.type != GUI_EVENT_KEY) continue;

            if (ev.a == 27 || ev.a == 'q' || ev.a == 'Q') {
                running = 0;
                break;
            }
            if (ev.a == 'r' || ev.a == 'R') {
                reset_snake(&st);
                continue;
            }
            if (ev.a == 'p' || ev.a == 'P') {
                st.paused = !st.paused;
                continue;
            }

            if (ev.a == GUI_KEY_UP || ev.a == 'w' || ev.a == 'W') st.next_dir = 0;
            if (ev.a == GUI_KEY_RIGHT || ev.a == 'd' || ev.a == 'D') st.next_dir = 1;
            if (ev.a == GUI_KEY_DOWN || ev.a == 's' || ev.a == 'S') st.next_dir = 2;
            if (ev.a == GUI_KEY_LEFT || ev.a == 'a' || ev.a == 'A') st.next_dir = 3;
        }

        ++st.frame_counter;
        if (st.frame_counter >= tick_interval(&st)) {
            st.frame_counter = 0;
            step_snake(&st);
        }

        draw_snake(h, &st);
        usleep(16000);
    }

    (void)gui_set_continuous_redraw(h, 0);
    return 0;
}
