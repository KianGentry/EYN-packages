#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Create or remove command aliases.", "alias ll ls -l");

#define ALIAS_CFG_PATH "/config/aliases.cfg"
#define ALIAS_MAX_COUNT 32
#define ALIAS_MAX_NAME 64
#define ALIAS_MAX_TMPL 200
#define ALIAS_FILE_MAX 4096

typedef struct {
    char name[ALIAS_MAX_NAME];
    char tmpl[ALIAS_MAX_TMPL];
} alias_entry_t;

static int find_alias(const alias_entry_t entries[ALIAS_MAX_COUNT], int count, const char* name);

static int is_ws(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static int is_name_char(char c) {
    if (c >= 'a' && c <= 'z') return 1;
    if (c >= 'A' && c <= 'Z') return 1;
    if (c >= '0' && c <= '9') return 1;
    return c == '_';
}

static int alias_normalize_key(const char* in, char* out, int out_cap) {
    if (!in || !out || out_cap <= 1) return -1;

    int pos = 0;
    int saw_word = 0;
    int pending_space = 0;
    int i = 0;

    while (in[i] && is_ws(in[i])) i++;
    while (in[i]) {
        char c = in[i++];
        if (is_ws(c)) {
            pending_space = 1;
            continue;
        }
        if (!is_name_char(c)) return -1;

        if (pending_space && saw_word) {
            if (pos + 1 >= out_cap) return -1;
            out[pos++] = ' ';
        }

        if (pos + 1 >= out_cap) return -1;
        out[pos++] = c;
        saw_word = 1;
        pending_space = 0;
    }

    out[pos] = '\0';
    return saw_word ? 0 : -1;
}

static int alias_has_space(const char* name) {
    if (!name) return 0;
    for (int i = 0; name[i]; ++i) {
        if (is_ws(name[i])) return 1;
    }
    return 0;
}

static int contains_meta_chars(const char* s) {
    if (!s) return 1;
    for (int i = 0; s[i]; ++i) {
        if (s[i] == '|' || s[i] == '<' || s[i] == '>' || s[i] == '&') return 1;
    }
    return 0;
}

static int trim_bounds(const char* s, int start, int end, int* out_s, int* out_e) {
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n')) start++;
    while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\r' || s[end - 1] == '\n')) end--;
    *out_s = start;
    *out_e = end;
    return (start < end) ? 0 : -1;
}

static int alias_builtin_exists(const char* name) {
    char path[128];
    path[0] = '\0';
    strcpy(path, "/binaries/");
    if ((strlen(path) + strlen(name)) >= sizeof(path)) return 0;
    strcat(path, name);
    int fd = open(path, O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        return 1;
    }
    return 0;
}

static int parse_config_line_bounds(char* buf,
                                    int s,
                                    int e,
                                    char* out_name,
                                    int out_name_cap,
                                    char* out_tmpl,
                                    int out_tmpl_cap) {
    if (!buf || !out_name || !out_tmpl) return -1;
    if (trim_bounds(buf, s, e, &s, &e) != 0) return -1;
    if (buf[s] == '#') return -1;

    int ns = 0, ne = 0, ts = 0, te = 0;
    if (buf[s] == '"') {
        int q = s + 1;
        while (q < e && buf[q] != '"') q++;
        if (q >= e) return -1;
        ns = s + 1;
        ne = q;
        ts = q + 1;
        te = e;
        if (trim_bounds(buf, ns, ne, &ns, &ne) != 0) return -1;
        if (trim_bounds(buf, ts, te, &ts, &te) != 0) return -1;
        if (buf[ts] == '=') {
            ts++;
            if (trim_bounds(buf, ts, te, &ts, &te) != 0) return -1;
        }
    } else {
        int sep = -1;
        for (int k = s + 1; k < e - 1; ++k) {
            if (buf[k] == '=' && (buf[k - 1] == ' ' || buf[k - 1] == '\t') && (buf[k + 1] == ' ' || buf[k + 1] == '\t')) {
                sep = k;
                break;
            }
        }

        if (sep >= 0) {
            ns = s;
            ne = sep;
            ts = sep + 1;
            te = e;
            if (trim_bounds(buf, ns, ne, &ns, &ne) != 0) return -1;
            if (trim_bounds(buf, ts, te, &ts, &te) != 0) return -1;
        } else {
            int p = s;
            while (p < e && !is_ws(buf[p])) p++;
            if (p >= e) return -1;
            ns = s;
            ne = p;
            ts = p;
            te = e;
            if (trim_bounds(buf, ns, ne, &ns, &ne) != 0) return -1;
            if (trim_bounds(buf, ts, te, &ts, &te) != 0) return -1;
        }
    }

    int name_len = ne - ns;
    int tmpl_len = te - ts;
    if (name_len <= 0 || name_len >= ALIAS_MAX_NAME) return -1;
    if (tmpl_len <= 0 || tmpl_len >= out_tmpl_cap) return -1;

    char raw_name[ALIAS_MAX_NAME];
    memcpy(raw_name, buf + ns, (size_t)name_len);
    raw_name[name_len] = '\0';
    if (alias_normalize_key(raw_name, out_name, out_name_cap) != 0) return -1;

    memcpy(out_tmpl, buf + ts, (size_t)tmpl_len);
    out_tmpl[tmpl_len] = '\0';
    return 0;
}

static int join_tokens(char* out, int out_cap, char** argv, int start, int end_exclusive) {
    if (!out || out_cap <= 1 || !argv) return -1;

    int pos = 0;
    out[0] = '\0';
    for (int i = start; i < end_exclusive; ++i) {
        const char* a = argv[i] ? argv[i] : "";
        int need = (int)strlen(a) + ((i > start) ? 1 : 0);
        if (pos + need >= out_cap) return -1;
        if (i > start) out[pos++] = ' ';
        strcpy(out + pos, a);
        pos += (int)strlen(a);
    }

    out[pos] = '\0';
    return (pos > 0) ? 0 : -1;
}

static int find_equals_token(char** argv, int start, int argc) {
    for (int i = start; i < argc; ++i) {
        if (argv[i] && strcmp(argv[i], "=") == 0) return i;
    }
    return -1;
}

static int load_aliases(alias_entry_t entries[ALIAS_MAX_COUNT], int* out_count) {
    char buf[ALIAS_FILE_MAX];
    int fd = open(ALIAS_CFG_PATH, O_RDONLY, 0);
    *out_count = 0;
    if (fd < 0) return 0;

    int len = 0;
    while (len < (ALIAS_FILE_MAX - 1)) {
        int rc = (int)read(fd, buf + len, (size_t)(ALIAS_FILE_MAX - 1 - len));
        if (rc <= 0) break;
        len += rc;
    }
    close(fd);
    buf[len] = '\0';

    int line_start = 0;
    for (int i = 0; i <= len; ++i) {
        if (i < len && buf[i] != '\n') continue;

        int s = line_start;
        int e = i;
        line_start = i + 1;
        char name[ALIAS_MAX_NAME];
        char tmpl[ALIAS_MAX_TMPL];
        if (parse_config_line_bounds(buf, s, e, name, sizeof(name), tmpl, sizeof(tmpl)) != 0) continue;

        if (contains_meta_chars(tmpl)) continue;
        if (!alias_has_space(name) && alias_builtin_exists(name)) continue;
        if (*out_count >= ALIAS_MAX_COUNT) continue;
        if (find_alias(entries, *out_count, name) >= 0) continue;

        strncpy(entries[*out_count].name, name, ALIAS_MAX_NAME - 1);
        entries[*out_count].name[ALIAS_MAX_NAME - 1] = '\0';
        strncpy(entries[*out_count].tmpl, tmpl, ALIAS_MAX_TMPL - 1);
        entries[*out_count].tmpl[ALIAS_MAX_TMPL - 1] = '\0';
        (*out_count)++;
    }

    return 0;
}

static int save_aliases(const alias_entry_t entries[ALIAS_MAX_COUNT], int count) {
    char out[ALIAS_FILE_MAX];
    int pos = 0;
    for (int i = 0; i < count; ++i) {
        int quote_name = alias_has_space(entries[i].name);
        int need = (int)strlen(entries[i].name) + 1 + (int)strlen(entries[i].tmpl) + 1;
        if (quote_name) need += 2;
        if (pos + need >= (ALIAS_FILE_MAX - 1)) break;

        if (quote_name) out[pos++] = '"';
        strcpy(out + pos, entries[i].name);
        pos += (int)strlen(entries[i].name);
        if (quote_name) out[pos++] = '"';
        out[pos++] = ' ';
        strcpy(out + pos, entries[i].tmpl);
        pos += (int)strlen(entries[i].tmpl);
        out[pos++] = '\n';
    }
    out[pos] = '\0';
    return (writefile(ALIAS_CFG_PATH, out, (size_t)pos) >= 0) ? 0 : -1;
}

static int find_alias(const alias_entry_t entries[ALIAS_MAX_COUNT], int count, const char* name) {
    for (int i = 0; i < count; ++i) {
        if (strcmp(entries[i].name, name) == 0) return i;
    }
    return -1;
}

static void print_usage(void) {
    puts("Usage: alias <name> <template>");
    puts("       alias <multi word name> = <template>");
    puts("       alias remove <name>");
    puts("       alias remove <multi word name>");
}

int main(int argc, char** argv) {
    alias_entry_t entries[ALIAS_MAX_COUNT];
    int count = 0;

    if (argc >= 2 && argv[1] && strcmp(argv[1], "-h") == 0) {
        print_usage();
        return 0;
    }

    if (load_aliases(entries, &count) != 0) {
        puts("alias: failed to load aliases");
        return 1;
    }

    if (argc < 2 || !argv[1] || !argv[1][0]) {
        print_usage();
        return 1;
    }

    if (strcmp(argv[1], "remove") == 0) {
        if (argc < 3 || !argv[2] || !argv[2][0]) {
            print_usage();
            return 1;
        }

        char raw_name[ALIAS_MAX_NAME];
        char name[ALIAS_MAX_NAME];
        if (join_tokens(raw_name, sizeof(raw_name), argv, 2, argc) != 0 ||
            alias_normalize_key(raw_name, name, sizeof(name)) != 0) {
            puts("alias: invalid alias name");
            return 1;
        }

        int idx = find_alias(entries, count, name);
        if (idx < 0) {
            printf("alias: not found: %s\n", name);
            return 1;
        }
        for (int i = idx; i < count - 1; ++i) entries[i] = entries[i + 1];
        count--;
        if (save_aliases(entries, count) != 0) {
            puts("alias: failed to save aliases");
            return 1;
        }
        printf("Alias removed: %s\n", name);
        return 0;
    }

    if (argc < 3) {
        print_usage();
        return 1;
    }

    char name[ALIAS_MAX_NAME];
    char tmpl[ALIAS_MAX_TMPL];

    int eq_idx = find_equals_token(argv, 1, argc);
    if (eq_idx >= 0) {
        if (eq_idx <= 1 || eq_idx >= argc - 1) {
            print_usage();
            return 1;
        }

        char raw_name[ALIAS_MAX_NAME];
        if (join_tokens(raw_name, sizeof(raw_name), argv, 1, eq_idx) != 0 ||
            alias_normalize_key(raw_name, name, sizeof(name)) != 0) {
            puts("alias: invalid alias name");
            return 1;
        }

        if (join_tokens(tmpl, sizeof(tmpl), argv, eq_idx + 1, argc) != 0) {
            puts("alias: template too long");
            return 1;
        }
    } else {
        if (alias_normalize_key(argv[1], name, sizeof(name)) != 0) {
            puts("alias: invalid alias name");
            return 1;
        }

        if (join_tokens(tmpl, sizeof(tmpl), argv, 2, argc) != 0) {
            puts("alias: template too long");
            return 1;
        }
    }

    if (!name[0]) {
        puts("alias: invalid alias name");
        return 1;
    }
    if (!alias_has_space(name) && alias_builtin_exists(name)) {
        puts("alias: cannot override existing command");
        return 1;
    }
    if (find_alias(entries, count, name) >= 0) {
        puts("alias: alias already exists");
        return 1;
    }

    if (!tmpl[0] || contains_meta_chars(tmpl)) {
        puts("alias: invalid template");
        return 1;
    }

    if (count >= ALIAS_MAX_COUNT) {
        puts("alias: alias table full");
        return 1;
    }

    strncpy(entries[count].name, name, ALIAS_MAX_NAME - 1);
    entries[count].name[ALIAS_MAX_NAME - 1] = '\0';
    strncpy(entries[count].tmpl, tmpl, ALIAS_MAX_TMPL - 1);
    entries[count].tmpl[ALIAS_MAX_TMPL - 1] = '\0';
    count++;

    if (save_aliases(entries, count) != 0) {
        puts("alias: failed to save aliases");
        return 1;
    }

    printf("Alias set: %s\n", name);
    return 0;
}
