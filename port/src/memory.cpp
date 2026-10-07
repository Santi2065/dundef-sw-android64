#include <sys/mman.h>
#include <cstdarg>
#include <cstdio>
#include <vector>

#include "core.h"

extern "C" {
typedef void* mspace;
mspace create_mspace_with_base(void* base, size_t capacity, int locked);
void* mspace_malloc(mspace msp, size_t bytes);
void mspace_free(mspace msp, void* mem);
void* mspace_calloc(mspace msp, size_t n, size_t elem_size);
void* mspace_realloc(mspace msp, void* mem, size_t newsize);
void* mspace_memalign(mspace msp, size_t alignment, size_t bytes);
}

void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    __android_log_vprint(ANDROID_LOG_FATAL, "ddport", fmt, ap);
    va_end(ap);
    abort();
}

namespace mem {
static mspace heap;

// Largest hole below 4GB in our address space. ART keeps its heap and boot image down
// there too (compressed refs), so we take a free gap instead of a fixed address.
static std::pair<uintptr_t, size_t> find_low_gap() {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) fatal("cannot read /proc/self/maps");
    uintptr_t prev_end = 0x10000000, best = 0;
    size_t best_size = 0;
    char line[512];
    auto consider = [&](uintptr_t start) {
        if (start > prev_end && start - prev_end > best_size) {
            best = prev_end;
            best_size = start - prev_end;
        }
    };
    while (fgets(line, sizeof line, f)) {
        unsigned long s, e;
        if (sscanf(line, "%lx-%lx", &s, &e) != 2) continue;
        if (s >= 0x100000000UL) break;
        consider(s);
        if (e > prev_end) prev_end = e;
    }
    fclose(f);
    consider(0xFFFF0000UL);
    return {best, best_size};
}

void init() {
    auto [gap, gap_size] = find_low_gap();
    // ponytail: 1.25GB cap is plenty for a game built for 512MB-1GB phones; raise if heap runs out
    size_t size = std::min<size_t>(gap_size, 1280u << 20) & ~((1u << 20) - 1);
    uintptr_t start = (gap + (1u << 20) - 1) & ~uintptr_t((1u << 20) - 1);
    if (size < (512u << 20)) fatal("no low-4GB gap big enough (%zu MB)", gap_size >> 20);
    void* p = mmap(reinterpret_cast<void*>(start), size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED || reinterpret_cast<uintptr_t>(p) != start) fatal("arena mmap failed at %lx", start);
    heap = create_mspace_with_base(p, size, 1);
    LOGI("guest arena %p..%p (%zu MB)", p, static_cast<char*>(p) + size, size >> 20);
}

void* alloc(size_t n) { return mspace_malloc(heap, n); }
void* alloc_aligned(size_t a, size_t n) { return mspace_memalign(heap, a, n); }
void* calloc(size_t n, size_t sz) { return mspace_calloc(heap, n, sz); }
void* realloc(void* p, size_t n) { return mspace_realloc(heap, p, n); }
void free(void* p) { mspace_free(heap, p); }
u32 strdup(const char* s) {
    size_t n = strlen(s) + 1;
    void* d = alloc(n);
    memcpy(d, s, n);
    return g(d);
}
}  // namespace mem

// ---------------------------------------------------------------- ELF32 loader
namespace {
struct Ehdr { u8 ident[16]; u16 type, machine; u32 version, entry, phoff, shoff, flags; u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { u32 type, offset, vaddr, paddr, filesz, memsz, flags, align; };
struct Sym { u32 name, value, size; u8 info, other; u16 shndx; };
struct Rel { u32 offset, info; };
enum { PT_LOAD_ = 1, PT_DYNAMIC_ = 2, PT_ARM_EXIDX_ = 0x70000001 };
enum { DT_NULL_ = 0, DT_PLTRELSZ_ = 2, DT_HASH_ = 4, DT_STRTAB_ = 5, DT_SYMTAB_ = 6, DT_REL_ = 17, DT_RELSZ_ = 18,
       DT_INIT_ = 12, DT_JMPREL_ = 23, DT_INIT_ARRAY_ = 25, DT_INIT_ARRAYSZ_ = 27 };
}  // namespace

u32 GuestLib::sym(const char* name) const {
    auto* syms = mem::h<Sym>(symtab);
    for (u32 i = 1; i < nsyms; i++)
        if (syms[i].shndx && !strcmp(mem::h<char>(strtab + syms[i].name), name)) return base + syms[i].value;
    return 0;
}

GuestLib load_guest(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) fatal("cannot open guest lib %s", path);
    fseek(f, 0, SEEK_END);
    std::vector<u8> file(ftell(f));
    fseek(f, 0, SEEK_SET);
    if (fread(file.data(), 1, file.size(), f) != file.size()) fatal("short read %s", path);
    fclose(f);

    auto* eh = reinterpret_cast<Ehdr*>(file.data());
    if (memcmp(eh->ident, "\x7f" "ELF\x01", 5) || eh->machine != 40) fatal("%s is not an ARM32 ELF", path);
    auto* ph = reinterpret_cast<Phdr*>(file.data() + eh->phoff);

    u32 span = 0;
    for (int i = 0; i < eh->phnum; i++)
        if (ph[i].type == PT_LOAD_) span = std::max(span, ph[i].vaddr + ph[i].memsz);

    GuestLib lib;
    void* image = mem::alloc_aligned(0x10000, span);
    memset(image, 0, span);
    lib.base = mem::g(image);
    u32 dyn = 0;
    for (int i = 0; i < eh->phnum; i++) {
        if (ph[i].type == PT_LOAD_) memcpy(mem::h(lib.base + ph[i].vaddr), &file[ph[i].offset], ph[i].filesz);
        if (ph[i].type == PT_LOAD_ && (ph[i].flags & 1)) lib.code_end = lib.base + ph[i].vaddr + ph[i].memsz;
        if (ph[i].type == PT_DYNAMIC_) dyn = lib.base + ph[i].vaddr;
        if (ph[i].type == PT_ARM_EXIDX_) { lib.exidx = lib.base + ph[i].vaddr; lib.exidx_count = ph[i].memsz / 8; }
    }

    u32 rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, hash = 0, init = 0;
    for (auto* d = mem::h<s32>(dyn); d[0] != DT_NULL_; d += 2) {
        u32 v = d[1];
        switch (d[0]) {
            case DT_REL_: rel = lib.base + v; break;
            case DT_RELSZ_: relsz = v; break;
            case DT_JMPREL_: jmprel = lib.base + v; break;
            case DT_PLTRELSZ_: pltrelsz = v; break;
            case DT_SYMTAB_: lib.symtab = lib.base + v; break;
            case DT_STRTAB_: lib.strtab = lib.base + v; break;
            case DT_HASH_: hash = lib.base + v; break;
            case DT_INIT_: init = lib.base + v; break;
            case DT_INIT_ARRAY_: lib.init_array = lib.base + v; break;
            case DT_INIT_ARRAYSZ_: lib.init_count = v / 4; break;
        }
    }
    lib.nsyms = mem::rd32(hash + 4);  // nchain == number of symbols
    if (init) fatal("DT_INIT not supported");

    auto* syms = mem::h<Sym>(lib.symtab);
    std::vector<u32> resolved(lib.nsyms, ~0u);
    auto resolve = [&](u32 idx) {
        if (resolved[idx] != ~0u) return resolved[idx];
        const Sym& s = syms[idx];
        const char* name = mem::h<char>(lib.strtab + s.name);
        u32 a = s.shndx ? lib.base + s.value : import_address(name);
        if (!a && !s.shndx && (s.info >> 4) != 2 /*STB_WEAK*/) {
            if ((s.info & 0xf) == 1 /*STT_OBJECT*/) fatal("missing data import %s", name);
            a = make_thunk(nullptr, name);  // traps with the name if the game ever calls it
            LOGW("unimplemented import %s", name);
        }
        return resolved[idx] = a;
    };
    auto apply = [&](u32 table, u32 size) {
        for (auto* r = mem::h<Rel>(table); r < mem::h<Rel>(table + size); r++) {
            u32 type = r->info & 0xff, idx = r->info >> 8;
            u32 where = lib.base + r->offset;
            switch (type) {
                case 0: break;
                case 23: mem::wr32(where, mem::rd32(where) + lib.base); break;      // RELATIVE
                case 2: mem::wr32(where, mem::rd32(where) + resolve(idx)); break;  // ABS32
                case 21: case 22: mem::wr32(where, resolve(idx)); break;           // GLOB_DAT, JUMP_SLOT
                default: fatal("unsupported relocation type %u", type);
            }
        }
    };
    apply(rel, relsz);
    apply(jmprel, pltrelsz);
    LOGI("loaded %s at %08x (%u KB), %u symbols", path, lib.base, span >> 10, lib.nsyms);
    return lib;
}

void run_guest_constructors(const GuestLib& lib) {
    for (u32 i = 0; i < lib.init_count; i++) {
        u32 fn = mem::rd32(lib.init_array + 4 * i);
        if (fn && fn != ~0u) call_guest(fn, {});
    }
}
