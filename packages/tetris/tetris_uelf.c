#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <gui.h>

EYN_CMDMETA_V1("Play classic falling-block puzzle game.", "tetris");

#define TETRIS_W 10
#define TETRIS_H 20
#define TETRIS_CELL 12

typedef struct {
    int x;
    int y;
} block_cell_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

typedef struct {
    uint8_t board[TETRIS_H][TETRIS_W];
    int piece_kind;
    int piece_rot;
    int piece_x;
    int piece_y;
    int next_kind;
    int frame_counter;
    int lines;
    int score;
    int game_over;
    int paused;
    uint32_t rng;
} tetris_state_t;

static const block_cell_t k_piece_cells[7][4][4] = {
    {
        {{0, 1}, {1, 1}, {2, 1}, {3, 1}},
        {{2, 0}, {2, 1}, {2, 2}, {2, 3}},
        {{0, 2}, {1, 2}, {2, 2}, {3, 2}},
        {{1, 0}, {1, 1}, {1, 2}, {1, 3}}
    },
    {
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {2, 1}}
    },
    {
        {{1, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {1, 1}, {2, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {1, 2}},
        {{1, 0}, {0, 1}, {1, 1}, {1, 2}}
    },
    {
        {{1, 0}, {2, 0}, {0, 1}, {1, 1}},
        {{1, 0}, {1, 1}, {2, 1}, {2, 2}},
        {{1, 1}, {2, 1}, {0, 2}, {1, 2}},
        {{0, 0}, {0, 1}, {1, 1}, {1, 2}}
    },
    {
        {{0, 0}, {1, 0}, {1, 1}, {2, 1}},
        {{2, 0}, {1, 1}, {2, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {1, 2}, {2, 2}},
        {{1, 0}, {0, 1}, {1, 1}, {0, 2}}
    },
    {
        {{0, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {2, 0}, {1, 1}, {1, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {2, 2}},
        {{1, 0}, {1, 1}, {0, 2}, {1, 2}}
    },
    {
        {{2, 0}, {0, 1}, {1, 1}, {2, 1}},
        {{1, 0}, {1, 1}, {1, 2}, {2, 2}},
        {{0, 1}, {1, 1}, {2, 1}, {0, 2}},
        {{0, 0}, {1, 0}, {1, 1}, {1, 2}}
    }
};

static const rgb_t k_piece_colors[8] = {
    {28, 28, 32},
    {120, 220, 255},
    {250, 220, 120},
    {185, 130, 255},
    {120, 225, 140},
    {250, 130, 130},
    {120, 165, 255},
    {255, 170, 120}
};

static uint32_t rng_next(tetris_state_t* s) {
    s->rng = s->rng * 1664525u + 1013904223u;
    return s->rng;
}

static int level_from_lines(int lines) {
    int lvl = lines / 10;
    if (lvl > 12) lvl = 12;
    return lvl;
}

static int drop_interval_frames(const tetris_state_t* s) {
    int lvl = level_from_lines(s->lines);
    int interval = 34 - lvl * 2;
    if (interval < 7) interval = 7;
    return interval;
}

static int piece_collides(const tetris_state_t* s, int test_x, int test_y, int test_rot) {
    for (int i = 0; i < 4; ++i) {
        int bx = test_x + k_piece_cells[s->piece_kind][test_rot][i].x;
        int by = test_y + k_piece_cells[s->piece_kind][test_rot][i].y;
        if (bx < 0 || bx >= TETRIS_W) return 1;
        if (by >= TETRIS_H) return 1;
        if (by >= 0 && s->board[by][bx] != 0) return 1;
    }
    return 0;
}

static void spawn_piece(tetris_state_t* s) {
    s->piece_kind = s->next_kind;
    s->next_kind = (int)(rng_next(s) % 7u);
    s->piece_rot = 0;
    s->piece_x = 3;
    s->piece_y = 0;
    if (piece_collides(s, s->piece_x, s->piece_y, s->piece_rot)) {
        s->game_over = 1;
    }
}

static void reset_game(tetris_state_t* s) {
    memset(s, 0, sizeof(*s));
    s->rng = 0x4B1D1234u;
    s->next_kind = (int)(rng_next(s) % 7u);
    spawn_piece(s);
}

static void lock_piece(tetris_state_t* s) {
    for (int i = 0; i < 4; ++i) {
        int bx = s->piece_x + k_piece_cells[s->piece_kind][s->piece_rot][i].x;
        int by = s->piece_y + k_piece_cells[s->piece_kind][s->piece_rot][i].y;
        if (bx >= 0 && bx < TETRIS_W && by >= 0 && by < TETRIS_H) {
            s->board[by][bx] = (uint8_t)(s->piece_kind + 1);
        }
    }
}

static int clear_completed_lines(tetris_state_t* s) {
    int cleared = 0;
    for (int y = TETRIS_H - 1; y >= 0; --y) {
        int full = 1;
        for (int x = 0; x < TETRIS_W; ++x) {
            if (s->board[y][x] == 0) {
                full = 0;
                break;
            }
        }
        if (!full) continue;

        ++cleared;
        for (int yy = y; yy > 0; --yy) {
            memcpy(s->board[yy], s->board[yy - 1], TETRIS_W);
        }
        memset(s->board[0], 0, TETRIS_W);
        ++y;
    }

    if (cleared > 0) {
        static const int k_line_score[5] = {0, 100, 300, 500, 800};
        s->lines += cleared;
        s->score += k_line_score[cleared];
    }
    return cleared;
}

static void try_move_piece(tetris_state_t* s, int dx) {
    if (!piece_collides(s, s->piece_x + dx, s->piece_y, s->piece_rot)) {
        s->piece_x += dx;
    }
}

static void try_rotate_piece(tetris_state_t* s) {
    int test_rot = (s->piece_rot + 1) & 3;
    if (!piece_collides(s, s->piece_x, s->piece_y, test_rot)) {
        s->piece_rot = test_rot;
        return;
    }
    if (!piece_collides(s, s->piece_x - 1, s->piece_y, test_rot)) {
        s->piece_x -= 1;
        s->piece_rot = test_rot;
        return;
    }
    if (!piece_collides(s, s->piece_x + 1, s->piece_y, test_rot)) {
        s->piece_x += 1;
        s->piece_rot = test_rot;
    }
}

static void settle_and_spawn(tetris_state_t* s) {
    lock_piece(s);
    clear_completed_lines(s);
    spawn_piece(s);
}

static void hard_drop_piece(tetris_state_t* s) {
    int drop = 0;
    while (!piece_collides(s, s->piece_x, s->piece_y + 1, s->piece_rot)) {
        s->piece_y += 1;
        ++drop;
    }
    s->score += drop * 2;
    settle_and_spawn(s);
}

static void tick_fall(tetris_state_t* s) {
    if (!piece_collides(s, s->piece_x, s->piece_y + 1, s->piece_rot)) {
        s->piece_y += 1;
        return;
    }
    settle_and_spawn(s);
}

static void draw_block(int h, int px, int py, int size, const rgb_t* c) {
    gui_rect_t cell = {
        .x = px,
        .y = py,
        .w = size - 1,
        .h = size - 1,
        .r = c->r,
        .g = c->g,
        .b = c->b,
        ._pad = 0
    };
    (void)gui_fill_rect(h, &cell);
}

static void draw_tetris(int h, const tetris_state_t* s) {
    gui_size_t sz = {0, 0};
    (void)gui_get_content_size(h, &sz);
    if (sz.w <= 0) sz.w = 460;
    if (sz.h <= 0) sz.h = 330;

    int board_px_w = TETRIS_W * TETRIS_CELL;
    int board_px_h = TETRIS_H * TETRIS_CELL;
    int ox = 16;
    int oy = 18;

    (void)gui_begin(h);

    gui_rgb_t bg = {GUI_PAL_BG_R, GUI_PAL_BG_G, GUI_PAL_BG_B, 0};
    (void)gui_clear(h, &bg);

    gui_rect_t playfield = {
        .x = ox - 2,
        .y = oy - 2,
        .w = board_px_w + 4,
        .h = board_px_h + 4,
        .r = GUI_PAL_HEADER_R,
        .g = GUI_PAL_HEADER_G,
        .b = GUI_PAL_HEADER_B,
        ._pad = 0
    };
    (void)gui_fill_rect(h, &playfield);

    for (int y = 0; y < TETRIS_H; ++y) {
        for (int x = 0; x < TETRIS_W; ++x) {
            uint8_t v = s->board[y][x];
            if (v == 0) {
                rgb_t cell_bg = {36, 36, 42};
                draw_block(h, ox + x * TETRIS_CELL, oy + y * TETRIS_CELL, TETRIS_CELL, &cell_bg);
            } else {
                draw_block(h,
                           ox + x * TETRIS_CELL,
                           oy + y * TETRIS_CELL,
                           TETRIS_CELL,
                           &k_piece_colors[v]);
            }
        }
    }

    if (!s->game_over) {
        for (int i = 0; i < 4; ++i) {
            int bx = s->piece_x + k_piece_cells[s->piece_kind][s->piece_rot][i].x;
            int by = s->piece_y + k_piece_cells[s->piece_kind][s->piece_rot][i].y;
            if (bx < 0 || bx >= TETRIS_W || by < 0 || by >= TETRIS_H) continue;
            draw_block(h,
                       ox + bx * TETRIS_CELL,
                       oy + by * TETRIS_CELL,
                       TETRIS_CELL,
                       &k_piece_colors[s->piece_kind + 1]);
        }
    }

    char line1[96];
    char line2[96];
    char line3[96];
    int level = level_from_lines(s->lines) + 1;
    snprintf(line1, sizeof(line1), "Score: %d", s->score);
    snprintf(line2, sizeof(line2), "Lines: %d  Level: %d", s->lines, level);
    snprintf(line3, sizeof(line3), "Next: %d", s->next_kind + 1);

    int tx = ox + board_px_w + 18;
    gui_text_t t0 = {tx, oy + 2, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, 0, "Tetris"};
    gui_text_t t1 = {tx, oy + 22, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, 0, line1};
    gui_text_t t2 = {tx, oy + 34, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, 0, line2};
    gui_text_t t3 = {tx, oy + 46, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B, 0, line3};
    gui_text_t h1 = {tx, oy + 78, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B, 0, "Arrows: move/rotate"};
    gui_text_t h2 = {tx, oy + 90, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B, 0, "Space: hard drop"};
    gui_text_t h3 = {tx, oy + 102, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B, 0, "P pause, R restart"};
    gui_text_t h4 = {tx, oy + 114, GUI_PAL_DIM_R, GUI_PAL_DIM_G, GUI_PAL_DIM_B, 0, "Q/Esc quit"};

    (void)gui_draw_text(h, &t0);
    (void)gui_draw_text(h, &t1);
    (void)gui_draw_text(h, &t2);
    (void)gui_draw_text(h, &t3);
    (void)gui_draw_text(h, &h1);
    (void)gui_draw_text(h, &h2);
    (void)gui_draw_text(h, &h3);
    (void)gui_draw_text(h, &h4);

    if (s->paused) {
        gui_text_t msg = {tx, oy + 146, 255, 236, 145, 0, "Paused"};
        (void)gui_draw_text(h, &msg);
    }
    if (s->game_over) {
        gui_text_t msg1 = {tx, oy + 146, 255, 130, 130, 0, "Game Over"};
        gui_text_t msg2 = {tx, oy + 158, GUI_PAL_TEXT_R, GUI_PAL_TEXT_G, GUI_PAL_TEXT_B, 0, "Press R to play again"};
        (void)gui_draw_text(h, &msg1);
        (void)gui_draw_text(h, &msg2);
    }

    (void)gui_present(h);
}

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: tetris");
        return 0;
    }

    int h = gui_create("Tetris", "Arrows move, q quits");
    if (h < 0) h = gui_attach("Tetris", "Arrows move, q quits");
    if (h < 0) {
        puts("tetris: failed to initialize GUI");
        return 1;
    }

    (void)gui_set_continuous_redraw(h, 1);

    tetris_state_t st;
    reset_game(&st);

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
                reset_game(&st);
                continue;
            }
            if (ev.a == 'p' || ev.a == 'P') {
                st.paused = !st.paused;
                continue;
            }

            if (st.paused || st.game_over) continue;

            if (ev.a == GUI_KEY_LEFT || ev.a == 'a' || ev.a == 'A') {
                try_move_piece(&st, -1);
            } else if (ev.a == GUI_KEY_RIGHT || ev.a == 'd' || ev.a == 'D') {
                try_move_piece(&st, 1);
            } else if (ev.a == GUI_KEY_UP || ev.a == 'w' || ev.a == 'W') {
                try_rotate_piece(&st);
            } else if (ev.a == GUI_KEY_DOWN || ev.a == 's' || ev.a == 'S') {
                if (!piece_collides(&st, st.piece_x, st.piece_y + 1, st.piece_rot)) {
                    st.piece_y += 1;
                    st.score += 1;
                } else {
                    settle_and_spawn(&st);
                }
            } else if (ev.a == ' ') {
                hard_drop_piece(&st);
            }
        }

        if (!st.paused && !st.game_over) {
            ++st.frame_counter;
            if (st.frame_counter >= drop_interval_frames(&st)) {
                st.frame_counter = 0;
                tick_fall(&st);
            }
        }

        draw_tetris(h, &st);
        usleep(16000);
    }

    (void)gui_set_continuous_redraw(h, 0);
    return 0;
}
