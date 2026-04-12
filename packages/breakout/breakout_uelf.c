#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <gui.h>

EYN_CMDMETA_V1("Arcade brick-breaker with paddle controls.", "breakout");

#define BRICK_COLS 10
#define BRICK_ROWS 6
#define FP_ONE 256

typedef struct {
    int lives;
    int score;
    int level;
    int left_held;
    int right_held;
    int paused;
    int game_over;
    int paddle_x;
    int paddle_w;
    int paddle_y;
    int ball_x_fp;
    int ball_y_fp;
    int ball_vx_fp;
    int ball_vy_fp;
    int ball_stuck;
    int bricks_left;
    uint8_t bricks[BRICK_ROWS][BRICK_COLS];
} breakout_state_t;

static int iabs(int v) {
    return (v < 0) ? -v : v;
}

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void reset_bricks(breakout_state_t* s) {
    s->bricks_left = 0;
    for (int y = 0; y < BRICK_ROWS; ++y) {
        for (int x = 0; x < BRICK_COLS; ++x) {
            s->bricks[y][x] = 1;
            ++s->bricks_left;
        }
    }
}

static void reset_round(breakout_state_t* s, int w, int h) {
    s->paddle_w = 68;
    s->paddle_y = h - 22;
    s->paddle_x = (w - s->paddle_w) / 2;

    s->ball_x_fp = (s->paddle_x + s->paddle_w / 2) * FP_ONE;
    s->ball_y_fp = (s->paddle_y - 7) * FP_ONE;

    int speed = 190 + s->level * 16;
    if (speed > 360) speed = 360;

    s->ball_vx_fp = (s->level & 1) ? speed : -speed;
    s->ball_vy_fp = -speed;
    s->ball_stuck = 1;
}

static void reset_game(breakout_state_t* s, int w, int h) {
    memset(s, 0, sizeof(*s));
    s->lives = 3;
    s->level = 1;
    reset_bricks(s);
    reset_round(s, w, h);
}

static void draw_breakout(int h, const breakout_state_t* s, int cw, int ch) {
    const int play_left = 12;
    const int play_top = 26;
    const int play_right = cw - 12;
    const int play_bottom = ch - 10;

    const int brick_gap = 2;
    const int brick_w = (play_right - play_left - (BRICK_COLS - 1) * brick_gap) / BRICK_COLS;
    const int brick_h = 12;

    static const uint8_t row_r[BRICK_ROWS] = {235, 245, 120, 120, 120, 220};
    static const uint8_t row_g[BRICK_ROWS] = {100, 170, 210, 185, 150, 130};
    static const uint8_t row_b[BRICK_ROWS] = {100, 100, 110, 245, 255, 240};

    (void)gui_begin(h);

    gui_rgb_t bg = {GUI_PAL_BG_R, GUI_PAL_BG_G, GUI_PAL_BG_B, 0};
    (void)gui_clear(h, &bg);

    gui_rect_t frame = {
        .x = play_left - 2,
        .y = play_top - 2,
        .w = (play_right - play_left) + 4,
        .h = (play_bottom - play_top) + 4,
        .r = GUI_PAL_HEADER_R,
        .g = GUI_PAL_HEADER_G,
        .b = GUI_PAL_HEADER_B,
        ._pad = 0
    };
    (void)gui_fill_rect(h, &frame);

    gui_rect_t field = {
        .x = play_left,
        .y = play_top,
        .w = play_right - play_left,
        .h = play_bottom - play_top,
        .r = 30,
        .g = 30,
        .b = 36,
        ._pad = 0
    };
    (void)gui_fill_rect(h, &field);

    for (int by = 0; by < BRICK_ROWS; ++by) {
        for (int bx = 0; bx < BRICK_COLS; ++bx) {
            if (!s->bricks[by][bx]) continue;
            gui_rect_t brick = {
                .x = play_left + bx * (brick_w + brick_gap),
                .y = play_top + 8 + by * (brick_h + brick_gap),
                .w = brick_w,
                .h = brick_h,
                .r = row_r[by],
                .g = row_g[by],
                .b = row_b[by],
                ._pad = 0
            };
            (void)gui_fill_rect(h, &brick);
        }
    }

    gui_rect_t paddle = {
        .x = s->paddle_x,
        .y = s->paddle_y,
        .w = s->paddle_w,
        .h = 8,
        .r = 210,
        .g = 218,
        .b = 238,
        ._pad = 0
    };
    (void)gui_fill_rect(h, &paddle);

    int ball_x = s->ball_x_fp / FP_ONE;
    int ball_y = s->ball_y_fp / FP_ONE;
    gui_rect_t ball = {
        .x = ball_x - 3,
        .y = ball_y - 3,
        .w = 6,
        .h = 6,
        .r = 255,
        .g = 238,
        .b = 145,
        ._pad = 0
    };
    (void)gui_fill_rect(h, &ball);

    char status[128];
    snprintf(status, sizeof(status), "Breakout  Score:%d  Lives:%d  Level:%d", s->score, s->lives, s->level);
    gui_text_t t0 = {14, 8, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, 0, status};
    gui_text_t t1 = {14, ch - 18, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B, 0, "Arrows/A-D move  Space launch  P pause  R restart  Q quit"};
    (void)gui_draw_text(h, &t0);
    (void)gui_draw_text(h, &t1);

    if (s->paused) {
        gui_text_t msg = {cw / 2 - 24, ch / 2 - 6, 255, 236, 145, 0, "Paused"};
        (void)gui_draw_text(h, &msg);
    } else if (s->game_over) {
        gui_text_t msg1 = {cw / 2 - 42, ch / 2 - 8, 255, 130, 130, 0, "Game Over"};
        gui_text_t msg2 = {cw / 2 - 98, ch / 2 + 8, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, 0, "Press R to start a new run"};
        (void)gui_draw_text(h, &msg1);
        (void)gui_draw_text(h, &msg2);
    } else if (s->ball_stuck) {
        gui_text_t msg = {cw / 2 - 92, ch / 2, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, 0, "Press Space to launch"};
        (void)gui_draw_text(h, &msg);
    }

    (void)gui_present(h);
}

static void update_breakout(breakout_state_t* s, int cw, int ch) {
    if (s->paused || s->game_over) return;

    const int play_left = 12;
    const int play_top = 26;
    const int play_right = cw - 12;
    const int play_bottom = ch - 10;

    const int ball_r = 3;
    const int paddle_speed = 7;

    const int brick_gap = 2;
    const int brick_w = (play_right - play_left - (BRICK_COLS - 1) * brick_gap) / BRICK_COLS;
    const int brick_h = 12;

    if (s->left_held) s->paddle_x -= paddle_speed;
    if (s->right_held) s->paddle_x += paddle_speed;
    s->paddle_x = clampi(s->paddle_x, play_left, play_right - s->paddle_w);

    if (s->ball_stuck) {
        s->ball_x_fp = (s->paddle_x + s->paddle_w / 2) * FP_ONE;
        s->ball_y_fp = (s->paddle_y - 7) * FP_ONE;
        return;
    }

    int prev_y = s->ball_y_fp / FP_ONE;

    s->ball_x_fp += s->ball_vx_fp;
    s->ball_y_fp += s->ball_vy_fp;

    int ball_x = s->ball_x_fp / FP_ONE;
    int ball_y = s->ball_y_fp / FP_ONE;

    if (ball_x - ball_r <= play_left) {
        ball_x = play_left + ball_r;
        s->ball_x_fp = ball_x * FP_ONE;
        s->ball_vx_fp = iabs(s->ball_vx_fp);
    } else if (ball_x + ball_r >= play_right) {
        ball_x = play_right - ball_r;
        s->ball_x_fp = ball_x * FP_ONE;
        s->ball_vx_fp = -iabs(s->ball_vx_fp);
    }

    if (ball_y - ball_r <= play_top) {
        ball_y = play_top + ball_r;
        s->ball_y_fp = ball_y * FP_ONE;
        s->ball_vy_fp = iabs(s->ball_vy_fp);
    }

    if (ball_y + ball_r >= play_bottom) {
        --s->lives;
        if (s->lives <= 0) {
            s->game_over = 1;
        } else {
            reset_round(s, cw, ch);
        }
        return;
    }

    if (s->ball_vy_fp > 0 && prev_y + ball_r <= s->paddle_y && ball_y + ball_r >= s->paddle_y) {
        if (ball_x >= s->paddle_x - ball_r && ball_x <= s->paddle_x + s->paddle_w + ball_r) {
            int center = s->paddle_x + s->paddle_w / 2;
            int offset = ball_x - center;
            s->ball_vx_fp = offset * 8;
            s->ball_vx_fp = clampi(s->ball_vx_fp, -330, 330);
            s->ball_vy_fp = -iabs(s->ball_vy_fp);
            s->ball_y_fp = (s->paddle_y - ball_r - 1) * FP_ONE;
        }
    }

    int collision_found = 0;
    for (int by = 0; by < BRICK_ROWS && !collision_found; ++by) {
        for (int bx = 0; bx < BRICK_COLS && !collision_found; ++bx) {
            if (!s->bricks[by][bx]) continue;
            int rx = play_left + bx * (brick_w + brick_gap);
            int ry = play_top + 8 + by * (brick_h + brick_gap);
            int rw = brick_w;
            int rh = brick_h;

            int near_x = clampi(ball_x, rx, rx + rw - 1);
            int near_y = clampi(ball_y, ry, ry + rh - 1);
            int dx = ball_x - near_x;
            int dy = ball_y - near_y;
            if (dx * dx + dy * dy > ball_r * ball_r) continue;

            s->bricks[by][bx] = 0;
            --s->bricks_left;
            s->score += 10;

            if (iabs(dx) > iabs(dy)) {
                s->ball_vx_fp = -s->ball_vx_fp;
            } else {
                s->ball_vy_fp = -s->ball_vy_fp;
            }
            collision_found = 1;
        }
    }

    if (s->bricks_left <= 0) {
        ++s->level;
        reset_bricks(s);
        reset_round(s, cw, ch);
    }
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: breakout");
        return 0;
    }

    int h = gui_create("Breakout", "Arrows move, q quits");
    if (h < 0) h = gui_attach("Breakout", "Arrows move, q quits");
    if (h < 0) {
        puts("breakout: failed to initialize GUI");
        return 1;
    }

    (void)gui_set_continuous_redraw(h, 1);

    gui_size_t sz = {0, 0};
    (void)gui_get_content_size(h, &sz);
    if (sz.w <= 0) sz.w = 460;
    if (sz.h <= 0) sz.h = 320;

    breakout_state_t st;
    reset_game(&st, sz.w, sz.h);

    int running = 1;
    while (running) {
        (void)gui_get_content_size(h, &sz);
        if (sz.w <= 0) sz.w = 460;
        if (sz.h <= 0) sz.h = 320;

        gui_event_t ev;
        while (gui_poll_event(h, &ev) > 0) {
            if (ev.type == GUI_EVENT_CLOSE) {
                running = 0;
                break;
            }

            if (ev.type == GUI_EVENT_KEY) {
                if (ev.a == 27 || ev.a == 'q' || ev.a == 'Q') {
                    running = 0;
                    break;
                }
                if (ev.a == 'r' || ev.a == 'R') {
                    reset_game(&st, sz.w, sz.h);
                    continue;
                }
                if (ev.a == 'p' || ev.a == 'P') {
                    st.paused = !st.paused;
                    continue;
                }
                if (ev.a == GUI_KEY_LEFT || ev.a == 'a' || ev.a == 'A') st.left_held = 1;
                if (ev.a == GUI_KEY_RIGHT || ev.a == 'd' || ev.a == 'D') st.right_held = 1;
                if (ev.a == ' ' && !st.game_over) st.ball_stuck = 0;
            } else if (ev.type == GUI_EVENT_KEY_UP) {
                if (ev.a == GUI_KEY_LEFT || ev.a == 'a' || ev.a == 'A') st.left_held = 0;
                if (ev.a == GUI_KEY_RIGHT || ev.a == 'd' || ev.a == 'D') st.right_held = 0;
            }
        }

        update_breakout(&st, sz.w, sz.h);
        draw_breakout(h, &st, sz.w, sz.h);
        usleep(16000);
    }

    (void)gui_set_continuous_redraw(h, 0);
    return 0;
}
