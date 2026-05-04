#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include <eynos_cmdmeta.h>

EYN_CMDMETA_V1("Minimal ELF dynamic loader for EYN-OS.", "ldso <program> [args...]");

#define EI_NIDENT   16
#define EI_MAG0     0
#define EI_MAG1     1
#define EI_MAG2     2
#define EI_MAG3     3
#define EI_CLASS    4
#define EI_DATA     5

#define ELFMAG0     0x7f
#define ELFMAG1     'E'
#define ELFMAG2     'L'
#define ELFMAG3     'F'

#define ELFCLASS32  1
#define ELFDATA2LSB 1
#define EM_386      3

#define ET_EXEC     2
#define ET_DYN      3

#define PT_LOAD     1
#define PT_DYNAMIC  2

#define DT_NULL       0
#define DT_NEEDED     1
#define DT_PLTRELSZ   2
#define DT_PLTGOT     3
#define DT_HASH       4
#define DT_STRTAB     5
#define DT_SYMTAB     6
#define DT_RELA       7
#define DT_RELASZ     8
#define DT_RELAENT    9
#define DT_STRSZ      10
#define DT_SYMENT     11
#define DT_REL        17
#define DT_RELSZ      18
#define DT_RELENT     19
#define DT_PLTREL     20
#define DT_JMPREL     23

#define R_386_NONE      0
#define R_386_32        1
#define R_386_PC32      2
#define R_386_GLOB_DAT  6
#define R_386_JMP_SLOT  7
#define R_386_RELATIVE  8

#define AT_NULL    0
#define AT_PHDR    3
#define AT_PHENT   4
#define AT_PHNUM   5
#define AT_PAGESZ  6
#define AT_BASE    7
#define AT_ENTRY   9

#define PAGE_SIZE 4096u
#define MAX_OBJECTS 8
#define MAX_NEEDED 8
#define MAX_PATH 256

typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Word;

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff;
    Elf32_Off e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

typedef struct {
    Elf32_Word d_tag;
    union {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
} Elf32_Dyn;

typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Half st_shndx;
} Elf32_Sym;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff;
    Elf32_Off e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} elf32_ehdr_t;

static inline uint32_t align_up32(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static inline uint32_t elf_r_sym(uint32_t info) {
    return info >> 8;
}

static inline uint32_t elf_r_type(uint32_t info) {
    return info & 0xffu;
}

typedef struct loaded_object loaded_object_t;

struct loaded_object {
    char path[MAX_PATH];
    uint8_t* file_data;
    size_t file_size;
    uint8_t* image;
    size_t image_size;
    uint32_t min_vaddr;
    uint32_t load_bias;
    Elf32_Ehdr* eh;
    Elf32_Phdr* phdr;
    Elf32_Dyn* dyn;
    Elf32_Sym* dynsym;
    const char* dynstr;
    uint32_t* sysv_hash;
    Elf32_Rel* rel;
    size_t rel_count;
    Elf32_Rel* plt_rel;
    size_t plt_rel_count;
    uint32_t dynstr_size;
    uint32_t sym_count;
    uint32_t entry_addr;
    uint32_t phdr_addr;
    uint16_t phnum;
    uint16_t phentsize;
    uint16_t plt_rel_type;
    uint32_t needed_offsets[MAX_NEEDED];
    char* needed[MAX_NEEDED];
    int needed_count;
    int loaded;
};

static loaded_object_t g_objects[MAX_OBJECTS];
static int g_object_count = 0;

static char* dup_string(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s) + 1;
    char* out = (char*)malloc(len);
    if (!out) return NULL;
    memcpy(out, s, len);
    return out;
}

static int file_read_all(const char* path, uint8_t** out_data, size_t* out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;

    long sz = lseek(fd, 0, SEEK_END);
    if (sz < 0) {
        close(fd);
        return -1;
    }
    if (lseek(fd, 0, SEEK_SET) < 0) {
        close(fd);
        return -1;
    }

    uint8_t* data = (uint8_t*)malloc((size_t)sz);
    if (!data) {
        close(fd);
        return -1;
    }

    size_t remaining = (size_t)sz;
    size_t offset = 0;
    while (remaining > 0) {
        ssize_t n = read(fd, data + offset, remaining);
        if (n <= 0) {
            free(data);
            close(fd);
            return -1;
        }
        offset += (size_t)n;
        remaining -= (size_t)n;
    }

    close(fd);
    *out_data = data;
    *out_size = (size_t)sz;
    return 0;
}

static loaded_object_t* find_loaded_object(const char* path) {
    for (int i = 0; i < g_object_count; ++i) {
        if (strcmp(g_objects[i].path, path) == 0) return &g_objects[i];
    }
    return NULL;
}

static const char* resolve_library_path(const char* needed) {
    static char candidate[MAX_PATH];

    if (!needed || !needed[0]) return NULL;
    if (strchr(needed, '/')) return needed;

    const char* prefixes[] = { "/binaries/", "/lib/", "." };
    for (int i = 0; i < 3; ++i) {
        if (strcmp(prefixes[i], ".") == 0) {
            snprintf(candidate, sizeof(candidate), "%s", needed);
        } else {
            snprintf(candidate, sizeof(candidate), "%s%s", prefixes[i], needed);
        }
        if (access(candidate, R_OK) == 0) return candidate;
    }

    return NULL;
}

static loaded_object_t* load_object(const char* path);

static uint32_t object_runtime_addr(const loaded_object_t* obj, uint32_t vaddr) {
    return obj->load_bias + vaddr;
}

static uint32_t resolve_symbol_addr(const char* name) {
    if (!name || !name[0]) return 0;

    for (int oi = 0; oi < g_object_count; ++oi) {
        loaded_object_t* obj = &g_objects[oi];
        if (!obj->loaded || !obj->dynsym || !obj->dynstr || obj->sym_count == 0) continue;

        for (uint32_t si = 0; si < obj->sym_count; ++si) {
            Elf32_Sym* sym = &obj->dynsym[si];
            if (sym->st_name == 0 || sym->st_shndx == 0) continue;
            const char* sym_name = obj->dynstr + sym->st_name;
            if (strcmp(sym_name, name) == 0) {
                return object_runtime_addr(obj, sym->st_value);
            }
        }
    }

    return 0;
}

static int apply_relocations(const loaded_object_t* obj, Elf32_Rel* rels, size_t rel_count) {
    if (!obj || !rels || rel_count == 0) return 0;

    for (size_t i = 0; i < rel_count; ++i) {
        Elf32_Rel* rel = &rels[i];
        uint32_t type = elf_r_type(rel->r_info);
        uint32_t sym_index = elf_r_sym(rel->r_info);
        uint32_t* where = (uint32_t*)(uintptr_t)object_runtime_addr(obj, rel->r_offset);
        uint32_t addend = *where;
        uint32_t sym_addr = 0;

        if (sym_index != 0 && obj->dynsym && obj->dynstr) {
            Elf32_Sym* sym = &obj->dynsym[sym_index];
            if (sym->st_name != 0) {
                sym_addr = resolve_symbol_addr(obj->dynstr + sym->st_name);
            }
        }

        switch (type) {
            case R_386_NONE:
                break;
            case R_386_RELATIVE:
                *where = obj->load_bias + addend;
                break;
            case R_386_32:
            case R_386_GLOB_DAT:
            case R_386_JMP_SLOT:
                if (sym_addr == 0 && type != R_386_32) {
                    /* leave zero for unresolved weak-ish references in this prototype */
                    break;
                }
                *where = sym_addr + addend;
                break;
            case R_386_PC32:
                *where = sym_addr + addend - (uint32_t)(uintptr_t)where;
                break;
            default:
                printf("ldso: unsupported relocation type %u in %s\n", type, obj->path);
                return -1;
        }
    }

    return 0;
}

static int load_needed_libraries(loaded_object_t* obj) {
    for (int i = 0; i < obj->needed_count; ++i) {
        const char* resolved = resolve_library_path(obj->needed[i]);
        if (!resolved) {
            printf("ldso: unable to locate dependency %s for %s\n", obj->needed[i], obj->path);
            return -1;
        }
        if (!load_object(resolved)) return -1;
    }
    return 0;
}

static loaded_object_t* load_object(const char* path) {
    if (!path || !path[0]) return NULL;

    loaded_object_t* existing = find_loaded_object(path);
    if (existing) return existing;
    if (g_object_count >= MAX_OBJECTS) return NULL;

    uint8_t* file_data = NULL;
    size_t file_size = 0;
    if (file_read_all(path, &file_data, &file_size) != 0) {
        printf("ldso: failed to read %s\n", path);
        return NULL;
    }

    if (file_size < sizeof(Elf32_Ehdr)) {
        free(file_data);
        printf("ldso: invalid ELF %s\n", path);
        return NULL;
    }

    Elf32_Ehdr* eh = (Elf32_Ehdr*)file_data;
    if (eh->e_ident[EI_MAG0] != ELFMAG0 || eh->e_ident[EI_MAG1] != ELFMAG1 || eh->e_ident[EI_MAG2] != ELFMAG2 || eh->e_ident[EI_MAG3] != ELFMAG3) {
        free(file_data);
        printf("ldso: not an ELF file: %s\n", path);
        return NULL;
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS32 || eh->e_ident[EI_DATA] != ELFDATA2LSB || eh->e_machine != EM_386) {
        free(file_data);
        printf("ldso: unsupported ELF format: %s\n", path);
        return NULL;
    }
    if (eh->e_phoff == 0 || eh->e_phentsize < sizeof(Elf32_Phdr) || eh->e_phnum == 0) {
        free(file_data);
        printf("ldso: missing program headers: %s\n", path);
        return NULL;
    }
    if ((uint64_t)eh->e_phoff + (uint64_t)eh->e_phnum * (uint64_t)eh->e_phentsize > (uint64_t)file_size) {
        free(file_data);
        printf("ldso: program headers out of range: %s\n", path);
        return NULL;
    }

    uint32_t min_vaddr = 0xffffffffu;
    uint32_t max_vaddr = 0;
    uint32_t dyn_vaddr = 0;
    uint32_t dyn_size = 0;

    Elf32_Phdr* ph = (Elf32_Phdr*)(file_data + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        if (ph->p_type == PT_LOAD) {
            if (ph->p_vaddr < min_vaddr) min_vaddr = ph->p_vaddr;
            uint32_t end = ph->p_vaddr + ph->p_memsz;
            if (end > max_vaddr) max_vaddr = end;
        } else if (ph->p_type == PT_DYNAMIC) {
            dyn_vaddr = ph->p_vaddr;
            dyn_size = ph->p_memsz;
        }
        ph = (Elf32_Phdr*)((uint8_t*)ph + eh->e_phentsize);
    }

    if (min_vaddr == 0xffffffffu || max_vaddr <= min_vaddr) {
        free(file_data);
        printf("ldso: no loadable segments: %s\n", path);
        return NULL;
    }

    size_t image_size = (size_t)align_up32(max_vaddr - min_vaddr, PAGE_SIZE);
    uint8_t* image = (uint8_t*)calloc(1, image_size);
    if (!image) {
        free(file_data);
        printf("ldso: out of memory loading %s\n", path);
        return NULL;
    }

    ph = (Elf32_Phdr*)(file_data + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        if (ph->p_type == PT_LOAD) {
            if (ph->p_offset + ph->p_filesz > file_size) {
                free(image);
                free(file_data);
                printf("ldso: segment out of range: %s\n", path);
                return NULL;
            }
            uint32_t dst_off = ph->p_vaddr - min_vaddr;
            memcpy(image + dst_off, file_data + ph->p_offset, ph->p_filesz);
        }
        ph = (Elf32_Phdr*)((uint8_t*)ph + eh->e_phentsize);
    }

    loaded_object_t* obj = &g_objects[g_object_count];
    memset(obj, 0, sizeof(*obj));
    strncpy(obj->path, path, sizeof(obj->path) - 1);
    obj->path[sizeof(obj->path) - 1] = '\0';
    obj->file_data = file_data;
    obj->file_size = file_size;
    obj->image = image;
    obj->image_size = image_size;
    obj->min_vaddr = min_vaddr;
    obj->load_bias = (uint32_t)(uintptr_t)image - min_vaddr;
    obj->eh = (Elf32_Ehdr*)file_data;
    obj->phdr = (Elf32_Phdr*)(file_data + eh->e_phoff);
    obj->phnum = eh->e_phnum;
    obj->phentsize = eh->e_phentsize;
    obj->entry_addr = obj->load_bias + eh->e_entry;
    obj->phdr_addr = (uint32_t)(uintptr_t)(file_data + eh->e_phoff);

    if (dyn_vaddr && dyn_size) {
        obj->dyn = (Elf32_Dyn*)(image + (dyn_vaddr - min_vaddr));
        uint32_t dyn_sym_vaddr = 0;
        uint32_t dyn_str_vaddr = 0;
        uint32_t dyn_hash_vaddr = 0;
        uint32_t dyn_rel_vaddr = 0;
        uint32_t dyn_relsz = 0;
        uint32_t dyn_jmprel_vaddr = 0;
        uint32_t dyn_pltrelsz = 0;
        uint32_t dyn_pltrel = DT_REL;
        uint32_t dyn_syment = sizeof(Elf32_Sym);
        uint32_t dyn_strsz = 0;

        for (Elf32_Dyn* d = obj->dyn; d->d_tag != DT_NULL; ++d) {
            switch (d->d_tag) {
                case DT_NEEDED:
                    if (obj->needed_count < MAX_NEEDED) {
                        obj->needed_offsets[obj->needed_count++] = d->d_un.d_val;
                    }
                    break;
                case DT_SYMTAB:
                    dyn_sym_vaddr = d->d_un.d_ptr;
                    break;
                case DT_STRTAB:
                    dyn_str_vaddr = d->d_un.d_ptr;
                    break;
                case DT_HASH:
                    dyn_hash_vaddr = d->d_un.d_ptr;
                    break;
                case DT_REL:
                    dyn_rel_vaddr = d->d_un.d_ptr;
                    break;
                case DT_RELSZ:
                    dyn_relsz = d->d_un.d_val;
                    break;
                case DT_JMPREL:
                    dyn_jmprel_vaddr = d->d_un.d_ptr;
                    break;
                case DT_PLTRELSZ:
                    dyn_pltrelsz = d->d_un.d_val;
                    break;
                case DT_PLTREL:
                    dyn_pltrel = d->d_un.d_val;
                    break;
                case DT_SYMENT:
                    dyn_syment = d->d_un.d_val;
                    break;
                case DT_STRSZ:
                    dyn_strsz = d->d_un.d_val;
                    break;
                default:
                    break;
            }
        }

        if (dyn_str_vaddr) obj->dynstr = (const char*)(image + (dyn_str_vaddr - min_vaddr));
        if (dyn_sym_vaddr) obj->dynsym = (Elf32_Sym*)(image + (dyn_sym_vaddr - min_vaddr));
        if (dyn_hash_vaddr) obj->sysv_hash = (uint32_t*)(image + (dyn_hash_vaddr - min_vaddr));
        if (dyn_rel_vaddr) {
            obj->rel = (Elf32_Rel*)(image + (dyn_rel_vaddr - min_vaddr));
            obj->rel_count = dyn_relsz / sizeof(Elf32_Rel);
        }
        if (dyn_jmprel_vaddr) {
            obj->plt_rel = (Elf32_Rel*)(image + (dyn_jmprel_vaddr - min_vaddr));
            obj->plt_rel_count = dyn_pltrelsz / sizeof(Elf32_Rel);
            obj->plt_rel_type = (uint16_t)dyn_pltrel;
        }
        obj->dynstr_size = dyn_strsz;

        if (obj->sysv_hash) {
            obj->sym_count = obj->sysv_hash[1];
        } else if (obj->dynsym && dyn_syment == sizeof(Elf32_Sym)) {
            /* Conservative fallback for small prototypes. */
            obj->sym_count = 256;
        }

        if (obj->dynstr) {
            for (int i = 0; i < obj->needed_count; ++i) {
                const char* needed_name = obj->dynstr + obj->needed_offsets[i];
                obj->needed[i] = dup_string(needed_name);
            }
        }
    }

    obj->loaded = 1;
    g_object_count++;

    if (load_needed_libraries(obj) != 0) {
        return NULL;
    }

    return obj;
}

static int relocate_loaded_objects(void) {
    for (int i = 0; i < g_object_count; ++i) {
        loaded_object_t* obj = &g_objects[i];
        if (obj->rel && obj->rel_count) {
            if (apply_relocations(obj, obj->rel, obj->rel_count) != 0) return -1;
        }
        if (obj->plt_rel && obj->plt_rel_count) {
            if (obj->plt_rel_type != DT_REL) {
                printf("ldso: unsupported PLT relocation format in %s\n", obj->path);
                return -1;
            }
            if (apply_relocations(obj, obj->plt_rel, obj->plt_rel_count) != 0) return -1;
        }
    }
    return 0;
}

static void* build_target_stack(const char* target_path, int argc, char** argv, loaded_object_t* main_obj) {
    const size_t stack_size = 16u * 1024u;
    uint8_t* stack = (uint8_t*)malloc(stack_size);
    if (!stack) return NULL;

    uintptr_t sp = (uintptr_t)stack + stack_size;
    uintptr_t arg_ptrs[32];
    int arg_count = 0;

    if (target_path && target_path[0]) {
        size_t len = strlen(target_path) + 1;
        sp -= len;
        memcpy((void*)sp, target_path, len);
        arg_ptrs[arg_count++] = sp;
    }

    for (int i = 2; i < argc && arg_count < 32; ++i) {
        const char* s = argv[i] ? argv[i] : "";
        size_t len = strlen(s) + 1;
        sp -= len;
        memcpy((void*)sp, s, len);
        arg_ptrs[arg_count++] = sp;
    }

    sp &= ~((uintptr_t)0xF);

    struct auxv_pair { uint32_t tag; uint32_t val; };
    struct auxv_pair auxv[8];
    int auxc = 0;
    auxv[auxc++] = (struct auxv_pair){ AT_PAGESZ, PAGE_SIZE };
    auxv[auxc++] = (struct auxv_pair){ AT_PHDR, main_obj ? main_obj->phdr_addr : 0 };
    auxv[auxc++] = (struct auxv_pair){ AT_PHENT, main_obj ? main_obj->phentsize : 0 };
    auxv[auxc++] = (struct auxv_pair){ AT_PHNUM, main_obj ? main_obj->phnum : 0 };
    auxv[auxc++] = (struct auxv_pair){ AT_ENTRY, main_obj ? main_obj->entry_addr : 0 };
    auxv[auxc++] = (struct auxv_pair){ AT_BASE, 0 };

    sp -= 8;
    ((uint32_t*)sp)[0] = AT_NULL;
    ((uint32_t*)sp)[1] = 0;
    for (int i = auxc - 1; i >= 0; --i) {
        sp -= 8;
        ((uint32_t*)sp)[0] = auxv[i].tag;
        ((uint32_t*)sp)[1] = auxv[i].val;
    }
    sp -= 4; *(uint32_t*)sp = 0; /* envp NULL */
    sp -= 4; *(uint32_t*)sp = 0; /* argv NULL */
    for (int i = arg_count - 1; i >= 0; --i) {
        sp -= 4;
        *(uint32_t*)sp = (uint32_t)arg_ptrs[i];
    }
    sp -= 4;
    *(uint32_t*)sp = (uint32_t)arg_count;

    return (void*)sp;
}

__attribute__((noreturn)) static void transfer_control(void* entry, void* stack_ptr) {
    __asm__ __volatile__(
        "movl %0, %%esp\n\t"
        "xorl %%ebp, %%ebp\n\t"
        "jmp *%1\n\t"
        :
        : "r"(stack_ptr), "r"(entry)
        : "memory"
    );
    __builtin_unreachable();
}

static void usage(void) {
    puts("Usage: ldso <program> [args...]");
}

int main(int argc, char** argv) {
    if (argc < 2 || !argv[1] || !argv[1][0]) {
        usage();
        return 1;
    }

    const char* target_path = argv[1];
    loaded_object_t* main_obj = load_object(target_path);
    if (!main_obj) {
        return 1;
    }

    if (relocate_loaded_objects() != 0) {
        return 1;
    }

    void* stack_ptr = build_target_stack(target_path, argc, argv, main_obj);
    if (!stack_ptr) {
        printf("ldso: unable to build initial stack for %s\n", target_path);
        return 1;
    }

    transfer_control((void*)(uintptr_t)main_obj->entry_addr, stack_ptr);
}