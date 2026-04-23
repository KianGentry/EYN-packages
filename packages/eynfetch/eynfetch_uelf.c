/*
 * eynfetch -- fastfetch-style system info display for EYN-OS
 *
 * Colour output uses the EYN-OS vterm RGB escape:
 *   byte 0xFF followed by R, G, B sets the foreground colour.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include <eynos_cmdmeta.h>
#include <eynos_syscall.h>

EYN_CMDMETA_V1("Display system information with ASCII art logo.", "eynfetch");

/* ------------------------------------------------------------------ */
/*  Colour helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_colour(uint8_t r, uint8_t g, uint8_t b) {
    uint8_t seq[4] = { 0xFF, r, g, b };
    write(1, seq, 4);
}

static void write_str(const char* s) {
    if (s) write(1, s, strlen(s));
}

static void write_nl(void) {
    write(1, "\n", 1);
}

/* Palette */
#define COL_RED      200,  40,  40
#define COL_GREEN     60, 180,  60
#define COL_BLUE      60, 140, 220
#define COL_LABEL     80, 200, 220   /* cyan-ish for labels  */
#define COL_VALUE    220, 220, 220   /* light grey for values */
#define COL_DEFAULT  200, 200, 200

/* ------------------------------------------------------------------ */
/*  ASCII art logo                                                       */
/*                                                                      */
/*  E = red pentagon/arrow    Y = green    N = blue                    */
/*  Each element of logo_line is { text, r, g, b }.                    */
/*  A NULL text pointer means end of segments for that line.           */
/* ------------------------------------------------------------------ */

typedef struct { const char* text; uint8_t r, g, b; } seg_t;

#define R COL_RED
#define G COL_GREEN
#define B COL_BLUE

/*
 * Logo is 9 lines tall.
 * Each line has up to 3 coloured segments (E, Y, N parts) + terminator.
 */
#define LOGO_LINES 9
#define MAX_SEGS   4   /* segments per line + null terminator */

static const seg_t logo[LOGO_LINES][MAX_SEGS] = {
    { {" #######", R},{"====    =====", G},{"+++++    +++++", B}, {NULL,0,0,0} },
    { {" ########", R},{"====  ====", G},{"+++++++   ++++  ", B}, {NULL,0,0,0} },
    { {" ##########", R},{"======", G},{"++++++++ +++++    ", B}, {NULL,0,0,0} },
    { {" ###########", R},{"====", G},{"++++ +++++++++     ", B}, {NULL,0,0,0} },
    { {" ##########", R},{"====", G},{"+++++  +++++++      ", B}, {NULL,0,0,0} },
    { {" ########", R},{"====", G},{"++++    ++++++        ", B}, {NULL,0,0,0} },
    { {" #######", R},{"====", G},{"++++     +++++         ", B}, {NULL,0,0,0} },
    { {"                                   ", COL_DEFAULT}, {NULL,0,0,0} },
    { {" EYN-OS Release 16                 ", COL_LABEL  }, {NULL,0,0,0} },
};

#undef R
#undef G
#undef B

/* Visual width of each logo line (chars, excluding colour escape bytes) */
#define LOGO_COL_W 36

/* ------------------------------------------------------------------ */
/*  Info gathering                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char os[64];
    char kernel[32];
    char shell[32];
    char uptime[48];
    char display[48];
    char net_status[16];
    char ip[24];
    char mac[24];
    char drives[16];
    char pci_count[16];
} fetch_info_t;

static void gather(fetch_info_t* f) {
    snprintf(f->os,     sizeof(f->os),     "EYN-OS Release 16");
    snprintf(f->kernel, sizeof(f->kernel),  "EYN/i386");
    snprintf(f->shell,  sizeof(f->shell),   "eynsh");

    /* Uptime */
    int ms = eyn_syscall0(EYN_SYSCALL_GET_TICKS_MS);
    if (ms >= 0) {
        unsigned int s = (unsigned int)ms / 1000u;
        unsigned int h = s / 3600u;
        unsigned int m = (s % 3600u) / 60u;
        s %= 60u;
        if (h)      snprintf(f->uptime, sizeof(f->uptime), "%uh %um %us", h, m, s);
        else if (m) snprintf(f->uptime, sizeof(f->uptime), "%um %us", m, s);
        else        snprintf(f->uptime, sizeof(f->uptime), "%us", s);
    } else {
        snprintf(f->uptime, sizeof(f->uptime), "unknown");
    }

    /* Display */
    eyn_display_mode_t dm;
    if (eyn_sys_get_display_mode(&dm) == 0)
        snprintf(f->display, sizeof(f->display), "%dx%d @ %dbpp",
                 dm.width, dm.height, dm.bpp);
    else
        snprintf(f->display, sizeof(f->display), "unknown");

    /* Network */
    if (eyn_sys_net_is_inited()) {
        snprintf(f->net_status, sizeof(f->net_status), "up");
        eyn_net_config_t cfg;
        if (eyn_sys_netcfg_get(&cfg) == 0)
            snprintf(f->ip, sizeof(f->ip), "%u.%u.%u.%u",
                     cfg.local_ip[0], cfg.local_ip[1],
                     cfg.local_ip[2], cfg.local_ip[3]);
        else
            snprintf(f->ip, sizeof(f->ip), "unknown");

        uint8_t mac[6];
        if (eyn_sys_net_get_mac(mac) == 0)
            snprintf(f->mac, sizeof(f->mac),
                     "%02x:%02x:%02x:%02x:%02x:%02x",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        else
            snprintf(f->mac, sizeof(f->mac), "unknown");
    } else {
        snprintf(f->net_status, sizeof(f->net_status), "down");
        snprintf(f->ip,         sizeof(f->ip),         "N/A");
        snprintf(f->mac,        sizeof(f->mac),        "N/A");
    }

    /* Drives */
    int dc = eyn_sys_drive_get_count();
    if (dc >= 0) snprintf(f->drives,    sizeof(f->drives),    "%d", dc);
    else         snprintf(f->drives,    sizeof(f->drives),    "unknown");

    /* PCI */
    int pc = eyn_sys_pci_get_count(0);
    if (pc >= 0) snprintf(f->pci_count, sizeof(f->pci_count), "%d", pc);
    else         snprintf(f->pci_count, sizeof(f->pci_count), "unknown");
}

/* ------------------------------------------------------------------ */
/*  Rendering                                                            */
/* ------------------------------------------------------------------ */

static void print_fetch(void) {
    fetch_info_t f;
    gather(&f);

    const char* labels[] = {
        "OS      ",
        "Kernel  ",
        "Shell   ",
        "Uptime  ",
        "Display ",
        "Network ",
        "IP      ",
        "MAC     ",
        "Drives  ",
        "PCI devs",
    };
    const char* values[] = {
        f.os,
        f.kernel,
        f.shell,
        f.uptime,
        f.display,
        f.net_status,
        f.ip,
        f.mac,
        f.drives,
        f.pci_count,
    };
    int nrows = (int)(sizeof(labels) / sizeof(labels[0]));

    int total = LOGO_LINES > nrows ? LOGO_LINES : nrows;

    for (int i = 0; i < total; i++) {

        /* --- Logo column --- */
        if (i < LOGO_LINES) {
            for (int s = 0; logo[i][s].text != NULL; s++) {
                set_colour(logo[i][s].r, logo[i][s].g, logo[i][s].b);
                write_str(logo[i][s].text);
            }
        } else {
            char pad[LOGO_COL_W + 1];
            memset(pad, ' ', LOGO_COL_W);
            pad[LOGO_COL_W] = '\0';
            set_colour(COL_DEFAULT);
            write_str(pad);
        }

        /* Separator */
        set_colour(COL_DEFAULT);
        write_str("  ");

        /* --- Info column --- */
        if (i < nrows) {
            set_colour(COL_LABEL);
            write_str(labels[i]);
            set_colour(COL_DEFAULT);
            write_str(": ");
            set_colour(COL_VALUE);
            write_str(values[i]);
        }

        set_colour(COL_DEFAULT);
        write_nl();
    }

    write_nl();
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                          */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv) {
    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        puts("Usage: eynfetch");
        puts("  Displays system information alongside the EYN-OS logo.");
        return 0;
    }

    print_fetch();
    return 0;
}