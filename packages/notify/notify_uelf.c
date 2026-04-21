#include <stdio.h>
#include <string.h>

#include <eynos_cmdmeta.h>
#include <notify.h>

EYN_CMDMETA_V1("Show a graphical notification popup.",
               "notify --title Update --message 'Updates available'");

typedef struct notify_opts {
    const char* title;
    const char* message;
    int level;
    int timeout_ms;
} notify_opts_t;

static void usage(void) {
    puts("Usage: notify [--title <text>] [--message <text>] [--level info|warning|error] [--timeout ms]");
    puts("Examples:");
    puts("  notify --message 'Package updates are available'");
    puts("  notify --title Update --message '3 updates available' --level warning --timeout 8000");
}

static int parse_positive_int(const char* text, int* out_value) {
    if (!text || !text[0] || !out_value) return -1;

    int value = 0;
    for (int i = 0; text[i]; i++) {
        if (text[i] < '0' || text[i] > '9') return -1;
        int next = value * 10 + (text[i] - '0');
        if (next < value) return -1;
        value = next;
    }

    *out_value = value;
    return 0;
}

static int parse_level(const char* text) {
    if (!text || !text[0]) return 0;
    if (strcmp(text, "warning") == 0 || strcmp(text, "warn") == 0) return 1;
    if (strcmp(text, "error") == 0) return 2;
    return 0;
}

int main(int argc, char** argv) {
    notify_opts_t opts;
    opts.title = "Notification";
    opts.message = "Event received.";
    opts.level = 0;
    opts.timeout_ms = 7000;

    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (!arg) continue;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage();
            return 0;
        }

        if (strcmp(arg, "--title") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            opts.title = argv[++i];
            continue;
        }

        if (strcmp(arg, "--message") == 0 || strcmp(arg, "--body") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            opts.message = argv[++i];
            continue;
        }

        if (strcmp(arg, "--level") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            opts.level = parse_level(argv[++i]);
            continue;
        }

        if (strcmp(arg, "--timeout") == 0) {
            if (i + 1 >= argc) {
                usage();
                return 1;
            }
            int parsed = 0;
            if (parse_positive_int(argv[++i], &parsed) != 0) {
                puts("notify: timeout must be a positive integer in milliseconds");
                return 1;
            }
            opts.timeout_ms = parsed;
            continue;
        }

        if (arg[0] == '-') {
            usage();
            return 1;
        }

        opts.message = arg;
    }

    if (!opts.title || !opts.title[0]) opts.title = "Notification";
    if (!opts.message || !opts.message[0]) opts.message = "Event received.";

    if (eyn_notify_post(opts.title,
                        opts.message,
                        (eyn_notify_level_t)opts.level,
                        (unsigned)opts.timeout_ms) != 0) {
        puts("notify: kernel toast post failed");
        return 1;
    }

    return 0;
}
