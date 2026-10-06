#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <zlib.h>

#include <cerrno>
#include <mutex>
#include <unordered_map>

#include "core.h"

extern GuestLib g_lib;
void init_pthread();

// printf with guest argument layout: long is 32-bit, doubles/long long are 8-byte aligned.
static std::string gformat(const char* f, VaReader va) {
    std::string out;
    char buf[512], spec[64];
    while (*f) {
        if (*f != '%') { out += *f++; continue; }
        const char* start = f++;
        if (*f == '%') { out += '%'; f++; continue; }
        std::string s = "%";
        while (strchr("-+ #0'", *f)) s += *f++;
        auto num = [&] {
            if (*f == '*') { f++; s += std::to_string(static_cast<s32>(va.w())); }
            else while (isdigit(static_cast<unsigned char>(*f))) s += *f++;
        };
        num();
        if (*f == '.') { s += *f++; num(); }
        int longs = 0;
        while (strchr("hlLqjzt", *f)) {
            if (*f == 'l' || *f == 'q' || *f == 'L') longs++;
            if (*f == 'j') longs = 2;
            f++;
        }
        char conv = *f ? *f++ : 0;
        switch (conv) {
            case 'd': case 'i':
                if (longs >= 2) snprintf(buf, sizeof buf, (s + "lld").c_str(), static_cast<long long>(va.dw()));
                else snprintf(buf, sizeof buf, (s + conv).c_str(), static_cast<s32>(va.w()));
                break;
            case 'u': case 'o': case 'x': case 'X':
                if (longs >= 2) snprintf(buf, sizeof buf, (s + "ll" + conv).c_str(), static_cast<unsigned long long>(va.dw()));
                else snprintf(buf, sizeof buf, (s + conv).c_str(), va.w());
                break;
            case 'c': snprintf(buf, sizeof buf, (s + 'c').c_str(), static_cast<int>(va.w())); break;
            case 'p': snprintf(buf, sizeof buf, "0x%x", va.w()); break;
            case 's': {
                u32 p = va.w();
                const char* str = p ? mem::h<char>(p) : "(null)";
                int n = snprintf(nullptr, 0, (s + 's').c_str(), str);
                std::string tmp(n + 1, '\0');
                snprintf(tmp.data(), n + 1, (s + 's').c_str(), str);
                tmp.resize(n);
                out += tmp;
                continue;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                u64 b = va.dw();
                double d;
                memcpy(&d, &b, 8);
                snprintf(buf, sizeof buf, (s + conv).c_str(), d);
                break;
            }
            case 'n': mem::wr32(va.w(), static_cast<u32>(out.size())); continue;
            default:
                snprintf(spec, sizeof spec, "%.*s", static_cast<int>(f - start), start);
                out += spec;
                continue;
        }
        out += buf;
    }
    return out;
}

// scanf: run the host scanf one conversion at a time ("<literal><spec>%n") and store
// each result with the guest's integer widths.
static int gscan(const char* in, FILE* fp, const char* f, VaReader va) {
    int assigned = 0, pos = 0;
    std::string lit;
    while (*f) {
        if (*f != '%' || f[1] == '%') {
            if (*f == '%') { lit += "%%"; f += 2; } else lit += *f++;
            continue;
        }
        const char* start = f++;
        bool suppress = *f == '*';
        if (suppress) f++;
        std::string width;
        while (isdigit(static_cast<unsigned char>(*f))) width += *f++;
        int longs = 0, halfs = 0;
        while (strchr("hlLqjzt", *f)) {
            if (*f == 'h') halfs++;
            if (*f == 'l' || *f == 'q' || *f == 'L') longs++;
            if (*f == 'j') longs = 2;
            f++;
        }
        std::string set;
        char conv = *f++;
        if (conv == '[') {
            set = "[";
            if (*f == '^') set += *f++;
            if (*f == ']') set += *f++;
            while (*f && *f != ']') set += *f++;
            if (*f) set += *f++;
        }
        std::string sp = lit + "%" + (suppress ? "*" : "") + width;
        lit.clear();
        int n = -1, r;
        auto run = [&](const std::string& fmt, void* dst) {
            std::string full = fmt + "%n";
            if (fp) return suppress ? fscanf(fp, full.c_str(), &n) : fscanf(fp, full.c_str(), dst, &n);
            return suppress ? sscanf(in + pos, full.c_str(), &n) : sscanf(in + pos, full.c_str(), dst, &n);
        };
        u32 dst;
        switch (conv) {
            case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
                long long v = 0;
                r = run(sp + "ll" + (conv == 'p' ? 'x' : conv), &v);
                if (!suppress && r >= 1) {
                    dst = va.w();
                    int size = longs >= 2 ? 8 : halfs >= 2 ? 1 : halfs ? 2 : 4;
                    memcpy(mem::h(dst), &v, size);
                }
                break;
            }
            case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
                double v = 0;
                r = run(sp + "lf", &v);
                if (!suppress && r >= 1) {
                    dst = va.w();
                    if (longs) memcpy(mem::h(dst), &v, 8);
                    else { float fv = static_cast<float>(v); memcpy(mem::h(dst), &fv, 4); }
                }
                break;
            }
            case 's': case 'c': case '[': {
                void* target = suppress ? nullptr : mem::h(va.w());
                r = run(sp + (conv == '[' ? set : std::string(1, conv)), target);
                break;
            }
            case 'n':
                if (!suppress) mem::wr32(va.w(), pos);
                continue;
            default:
                LOGW("scanf: unsupported conversion %.*s", static_cast<int>(f - start), start);
                return assigned;
        }
        if (n < 0) return assigned ? assigned : (r == EOF ? EOF : 0);
        if (!suppress) assigned++;
        pos += n;
    }
    return assigned;
}

// ---------------------------------------------------------------- FILE / DIR handles
namespace {
constexpr u32 kFileSize = 84;  // sizeof(FILE) in old 32-bit bionic, for &__sF[i]
u32 g_sF = 0;
std::mutex files_lock;
std::unordered_map<u32, FILE*> files;
std::unordered_map<u32, DIR*> dirs;
std::unordered_map<u32, u32> dirents;

FILE* hfile(u32 g) {
    std::lock_guard lk(files_lock);
    auto it = files.find(g);
    if (it == files.end()) fatal("bad guest FILE* %08x", g);
    return it->second;
}

int log_write(void*, const char* buf, int n) {
    __android_log_print(ANDROID_LOG_INFO, "ddguest", "%.*s", n, buf);
    return n;
}

u32 per_thread_buf(size_t n) {
    thread_local u32 b = 0;
    if (!b) b = mem::g(mem::alloc(4096));
    if (n > 4096) fatal("per-thread buffer too small");
    return b;
}

// guest struct tm: 9 ints, long tm_gmtoff, char* tm_zone (44 bytes)
void tm_to_guest(const tm& t, u32 g) {
    s32 v[11] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday,
                 t.tm_isdst, static_cast<s32>(t.tm_gmtoff), 0};
    memcpy(mem::h(g), v, 44);
}
tm tm_from_guest(u32 g) {
    s32 v[11];
    memcpy(v, mem::h(g), 44);
    tm t{};
    t.tm_sec = v[0]; t.tm_min = v[1]; t.tm_hour = v[2]; t.tm_mday = v[3]; t.tm_mon = v[4];
    t.tm_year = v[5]; t.tm_wday = v[6]; t.tm_yday = v[7]; t.tm_isdst = v[8]; t.tm_gmtoff = v[9];
    return t;
}

// guest struct stat (ARM EABI stat64 layout, 104 bytes)
void stat_to_guest(const struct stat& s, u32 g) {
    u8* p = mem::h<u8>(g);
    memset(p, 0, 104);
    auto put32 = [&](int off, u32 v) { memcpy(p + off, &v, 4); };
    auto put64 = [&](int off, u64 v) { memcpy(p + off, &v, 8); };
    put64(0, s.st_dev);
    put32(12, static_cast<u32>(s.st_ino));
    put32(16, s.st_mode);
    put32(20, s.st_nlink);
    put32(24, s.st_uid);
    put32(28, s.st_gid);
    put64(32, s.st_rdev);
    put64(48, s.st_size);
    put32(56, s.st_blksize);
    put64(64, s.st_blocks);
    put32(72, s.st_atim.tv_sec); put32(76, s.st_atim.tv_nsec);
    put32(80, s.st_mtim.tv_sec); put32(84, s.st_mtim.tv_nsec);
    put32(88, s.st_ctim.tv_sec); put32(92, s.st_ctim.tv_nsec);
    put64(96, s.st_ino);
}

int open_flags(u32 f) {
#if defined(__x86_64__)
    // ARM and x86 disagree on these four bits
    int h = f & ~0740000;
    if (f & 040000) h |= O_DIRECTORY;
    if (f & 0100000) h |= O_NOFOLLOW;
    if (f & 0200000) h |= O_DIRECT;
    return h;
#else
    return f;
#endif
}

timeval tv_from_guest(u32 g) { return {static_cast<s32>(mem::rd32(g)), static_cast<s32>(mem::rd32(g + 4))}; }
void tv_to_guest(const timeval& t, u32 g) { mem::wr32(g, t.tv_sec); mem::wr32(g + 4, t.tv_usec); }

// UE3 reads its whole config from Coalesced_<LANG>.bin: a stream of length-prefixed strings
// (negative length = UTF-16). We hand it a copy with MaxSmoothedFrameRate raised from 62, so
// the engine no longer caps itself at 60 fps on 120/144 Hz screens.
std::string utf16_fstring(const std::u16string& s) {
    s32 n = -static_cast<s32>(s.size() + 1);
    std::string out(reinterpret_cast<char*>(&n), 4);
    out.append(reinterpret_cast<const char*>(s.c_str()), (s.size() + 1) * 2);
    return out;
}
int open_coalesced(const char* path, int flags) {
    FILE* f = fopen(path, "rb");
    if (!f) return open(path, flags);
    std::string d;
    char buf[65536];
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) d.append(buf, n);
    fclose(f);
    const std::string key = utf16_fstring(u"MaxSmoothedFrameRate"), value = utf16_fstring(u"145");
    int patched = 0;
    for (size_t at = d.find(key); at != std::string::npos; at = d.find(key, at + 1)) {
        size_t v = at + key.size();
        s32 len;
        memcpy(&len, &d[v], 4);
        size_t vlen = 4 + (len < 0 ? -len * 2 : len);
        d.replace(v, vlen, value);
        patched++;
    }
    std::string out = std::string(path) + ".fps";
    if (FILE* w = fopen(out.c_str(), "wb")) {
        fwrite(d.data(), 1, d.size(), w);
        fclose(w);
        LOGI("%s: raised MaxSmoothedFrameRate in %d places", path, patched);
        return open(out.c_str(), flags);
    }
    return open(path, flags);
}

std::recursive_mutex guard_lock;
thread_local u32 qsort_cmp;
int qsort_tramp(const void* a, const void* b) { return static_cast<s32>(call_guest(qsort_cmp, {mem::g(a), mem::g(b)})); }

// guest addrinfo: flags, family, socktype, protocol, addrlen, canonname*, addr*, next* (32 bytes)
u32 addrinfo_to_guest(const addrinfo* a) {
    u32 head = 0, *link = &head;
    for (; a; a = a->ai_next) {
        u32 g = mem::g(mem::calloc(1, 32));
        u32 sa = mem::g(mem::alloc(a->ai_addrlen));
        memcpy(mem::h(sa), a->ai_addr, a->ai_addrlen);
        u32 v[8] = {static_cast<u32>(a->ai_flags), static_cast<u32>(a->ai_family), static_cast<u32>(a->ai_socktype),
                    static_cast<u32>(a->ai_protocol), a->ai_addrlen,
                    a->ai_canonname ? mem::strdup(a->ai_canonname) : 0, sa, 0};
        memcpy(mem::h(g), v, 32);
        *link = g;
        link = mem::h<u32>(g + 28);
    }
    return head;
}

// guest hostent: name*, aliases**, addrtype, length, addr_list** (20 bytes)
u32 hostent_to_guest(const hostent* he) {
    thread_local u32 g = 0;
    if (!he) return 0;
    if (!g) g = mem::g(mem::calloc(1, 20 + 64 * 4 + 64 * 16));
    u32 lists = g + 20, data = lists + 64 * 4, li = 0, d = data;
    auto add_list = [&](char** l, int len) {
        u32 start = lists + li * 4;
        for (int i = 0; l && l[i] && li < 60; i++) {
            u32 n = len ? len : strlen(l[i]) + 1;
            if (d + n > data + 64 * 16) break;
            memcpy(mem::h(d), l[i], n);
            mem::wr32(lists + 4 * li++, d);
            d += (n + 3) & ~3u;
        }
        mem::wr32(lists + 4 * li++, 0);
        return start;
    };
    u32 name = mem::strdup(he->h_name);  // ponytail: small leak per lookup, fine for a game
    u32 aliases = add_list(he->h_aliases, 0);
    u32 addrs = add_list(he->h_addr_list, he->h_length);
    u32 v[5] = {name, aliases, static_cast<u32>(he->h_addrtype), static_cast<u32>(he->h_length), addrs};
    memcpy(mem::h(g), v, 20);
    return g;
}
}  // namespace

// ---------------------------------------------------------------- registration
void init_libc() {
    init_pthread();

    // --- data symbols
    g_sF = mem::g(mem::calloc(3, kFileSize));
    files[g_sF] = stdin;
    files[g_sF + kFileSize] = files[g_sF + 2 * kFileSize] = funopen(nullptr, nullptr, log_write, nullptr, nullptr);
    setvbuf(files[g_sF + kFileSize], nullptr, _IOLBF, 0);
    register_data("__sF", g_sF);
    u32 guard = mem::g(mem::calloc(1, 4));
    mem::wr32(guard, 0x5f3759df);
    register_data("__stack_chk_guard", guard);
    register_data("__dso_handle", mem::g(mem::calloc(1, 4)));
    {
        // old BSD ctype: _ctype_ is a pointer to a 1+256 entry flag table (index c+1)
        u8* t = static_cast<u8*>(mem::calloc(1, 257));
        auto* lo = static_cast<s16*>(mem::calloc(257, 2));
        auto* up = static_cast<s16*>(mem::calloc(257, 2));
        lo[0] = up[0] = -1;
        for (int c = 0; c < 256; c++) {
            lo[c + 1] = c < 128 ? tolower(c) : c;
            up[c + 1] = c < 128 ? toupper(c) : c;
            if (c >= 128) continue;
            u8 fl = 0;
            if (isupper(c)) fl |= 0x01;
            if (islower(c)) fl |= 0x02;
            if (isdigit(c)) fl |= 0x04;
            if (isspace(c)) fl |= 0x08;
            if (ispunct(c)) fl |= 0x10;
            if (iscntrl(c)) fl |= 0x20;
            if (isxdigit(c) && !isdigit(c)) fl |= 0x40;
            if (c == ' ') fl |= 0x80;
            t[c + 1] = fl;
        }
        auto ptr_var = [](void* table) {
            u32 v = mem::g(mem::alloc(4));
            mem::wr32(v, mem::g(table));
            return v;
        };
        register_data("_ctype_", ptr_var(t));
        register_data("_tolower_tab_", ptr_var(lo));
        register_data("_toupper_tab_", ptr_var(up));
    }

    // --- memory / strings
    register_import("malloc", [](Ctx& c) {
        if (c.arg(0) >= (256u << 20)) LOGW("guest malloc(%u MB) from lr=%08x", c.arg(0) >> 20, c.r[14] - g_lib.base);
        c.ret(mem::g(mem::alloc(c.arg(0))));
    });
    register_import("calloc", [](Ctx& c) {
        if (u64(c.arg(0)) * c.arg(1) >= (256u << 20)) LOGW("guest calloc(%u x %u) from lr=%08x", c.arg(0), c.arg(1), c.r[14] - g_lib.base);
        c.ret(mem::g(mem::calloc(c.arg(0), c.arg(1))));
    });
    register_import("realloc", [](Ctx& c) {
        if (c.arg(1) >= (256u << 20)) LOGW("guest realloc(%u MB) from lr=%08x", c.arg(1) >> 20, c.r[14] - g_lib.base);
        c.ret(mem::g(mem::realloc(mem::h(c.arg(0)), c.arg(1))));
    });
    register_import("free", [](Ctx& c) { mem::free(mem::h(c.arg(0))); });
    IMPORT(memcpy); IMPORT(memmove); IMPORT(memset); IMPORT(memcmp);
    IMPORT(strlen); IMPORT(strcmp); IMPORT(strncmp); IMPORT(strcpy); IMPORT(strncpy); IMPORT(strcat);
    IMPORT(strspn); IMPORT(strcspn); IMPORT(strcasecmp); IMPORT(strncasecmp); IMPORT(wcslen);
    IMPORT_T(strchr, char*(const char*, int));
    IMPORT_T(strrchr, char*(const char*, int));
    IMPORT_T(strstr, char*(const char*, const char*));
    IMPORT(atoi); IMPORT(atoll);
    register_import("strtod", [](Ctx& c) {
        char* end;
        double d = strtod(mem::h<char>(c.arg(0)), &end);
        if (c.arg(1)) mem::wr32(c.arg(1), mem::g(end));
        write_ret(c, d);
    });
    register_import("strtoul", [](Ctx& c) {
        char* end;
        unsigned long long v = strtoull(mem::h<char>(c.arg(0)), &end, c.arg(2));
        if (c.arg(1)) mem::wr32(c.arg(1), mem::g(end));
        c.ret(static_cast<u32>(v));
    });
    register_import("strtoull", [](Ctx& c) {
        char* end;
        unsigned long long v = strtoull(mem::h<char>(c.arg(0)), &end, c.arg(2));
        if (c.arg(1)) mem::wr32(c.arg(1), mem::g(end));
        c.ret64(v);
    });
    register_import("qsort", [](Ctx& c) {
        u32 saved = qsort_cmp;
        qsort_cmp = c.arg(3);
        qsort(mem::h(c.arg(0)), c.arg(1), c.arg(2), qsort_tramp);
        qsort_cmp = saved;
    });
    IMPORT(lrand48); IMPORT(srand48);

    // --- math (softfp: floats/doubles travel in core registers)
    IMPORT_T(acos, double(double)); IMPORT_T(cos, double(double)); IMPORT_T(sin, double(double));
    IMPORT_T(exp, double(double)); IMPORT_T(floor, double(double)); IMPORT_T(log, double(double));
    IMPORT_T(log10, double(double)); IMPORT_T(sqrt, double(double)); IMPORT_T(fmod, double(double, double));
    IMPORT_T(pow, double(double, double)); IMPORT_T(ldexp, double(double, int)); IMPORT_T(frexp, double(double, int*));
    IMPORT(acosf); IMPORT(asinf); IMPORT(atan2f); IMPORT(atanf); IMPORT(ceilf); IMPORT(cosf); IMPORT(expf);
    IMPORT(floorf); IMPORT(fmodf); IMPORT(log10f); IMPORT(logf); IMPORT(powf); IMPORT(roundf); IMPORT(sinf);
    IMPORT(sqrtf); IMPORT(tanf);
    register_import("__isfinitef", [](Ctx& c) {
        float f;
        u32 b = c.arg(0);
        memcpy(&f, &b, 4);
        c.ret(isfinite(f));
    });

    // --- process / misc
    register_import("abort", [](Ctx& c) { fatal("guest abort() lr=%08x", c.r[14]); });
    register_import("exit", [](Ctx& c) { LOGI("guest exit(%d)", c.arg(0)); exit(c.arg(0)); });
    register_import("atexit", [](Ctx& c) { c.ret(0); });
    register_import("__aeabi_atexit", [](Ctx& c) { c.ret(0); });
    register_import("__stack_chk_fail", [](Ctx& c) { fatal("guest stack smashing detected lr=%08x", c.r[14]); });
    register_import("__cxa_guard_acquire", [](Ctx& c) {
        if (mem::rd32(c.arg(0)) & 1) return c.ret(0);
        guard_lock.lock();
        if (mem::rd32(c.arg(0)) & 1) { guard_lock.unlock(); return c.ret(0); }
        c.ret(1);
    });
    register_import("__cxa_guard_release", [](Ctx& c) {
        mem::wr32(c.arg(0), 1);
        guard_lock.unlock();
    });
    register_import("__errno", [](Ctx& c) { c.ret(guest_errno_addr()); });
    register_import("__gnu_Unwind_Find_exidx", [](Ctx& c) {
        mem::wr32(c.arg(1), g_lib.exidx_count);
        c.ret(g_lib.exidx);
    });
    register_import("dlopen", [](Ctx& c) {
        LOGI("guest dlopen(%s) -> refused", c.arg(0) ? mem::h<char>(c.arg(0)) : "NULL");
        c.ret(0);
    });
    register_import("dlsym", [](Ctx& c) { c.ret(0); });
    register_import("dlclose", [](Ctx& c) { c.ret(0); });
    IMPORT(getpid); IMPORT(sched_yield); IMPORT(usleep); IMPORT(pause);
    register_import("compress", [](Ctx& c) {
        uLongf n = mem::rd32(c.arg(1));
        int r = compress(mem::h<Bytef>(c.arg(0)), &n, mem::h<Bytef>(c.arg(2)), c.arg(3));
        mem::wr32(c.arg(1), n);
        c.ret(r);
    });
    register_import("uncompress", [](Ctx& c) {
        uLongf n = mem::rd32(c.arg(1));
        int r = uncompress(mem::h<Bytef>(c.arg(0)), &n, mem::h<Bytef>(c.arg(2)), c.arg(3));
        mem::wr32(c.arg(1), n);
        c.ret(r);
    });

    // --- time (guest time_t/long are 32-bit)
    register_import("time", [](Ctx& c) {
        u32 t = time(nullptr);
        if (c.arg(0)) mem::wr32(c.arg(0), t);
        c.ret(t);
    });
    IMPORT(clock);
    register_import("difftime", [](Ctx& c) { write_ret(c, static_cast<double>(static_cast<s32>(c.arg(0)) - static_cast<s32>(c.arg(1)))); });
    register_import("gettimeofday", [](Ctx& c) {
        timeval tv;
        int r = gettimeofday(&tv, nullptr);
        if (c.arg(0)) tv_to_guest(tv, c.arg(0));
        c.ret(r);
    });
    register_import("clock_gettime", [](Ctx& c) {
        timespec ts;
        int r = clock_gettime(c.arg(0), &ts);
        mem::wr32(c.arg(1), ts.tv_sec);
        mem::wr32(c.arg(1) + 4, ts.tv_nsec);
        c.ret(r);
    });
    register_import("gmtime_r", [](Ctx& c) {
        time_t t = static_cast<s32>(mem::rd32(c.arg(0)));
        tm r;
        gmtime_r(&t, &r);
        tm_to_guest(r, c.arg(1));
        c.ret(c.arg(1));
    });
    register_import("gmtime", [](Ctx& c) {
        thread_local u32 buf = mem::g(mem::alloc(44));
        time_t t = static_cast<s32>(mem::rd32(c.arg(0)));
        tm r;
        gmtime_r(&t, &r);
        tm_to_guest(r, buf);
        c.ret(buf);
    });
    register_import("localtime_r", [](Ctx& c) {
        time_t t = static_cast<s32>(mem::rd32(c.arg(0)));
        tm r;
        localtime_r(&t, &r);
        tm_to_guest(r, c.arg(1));
        c.ret(c.arg(1));
    });
    register_import("mktime", [](Ctx& c) {
        tm t = tm_from_guest(c.arg(0));
        time_t r = mktime(&t);
        tm_to_guest(t, c.arg(0));
        c.ret(static_cast<u32>(r));
    });
    register_import("ctime", [](Ctx& c) {
        time_t t = static_cast<s32>(mem::rd32(c.arg(0)));
        u32 b = per_thread_buf(64);
        ctime_r(&t, mem::h<char>(b));
        c.ret(b);
    });

    // --- files
    register_import("open", [](Ctx& c) {
        const char* path = mem::h<char>(c.arg(0));
        int flags = open_flags(c.arg(1));
        if (strstr(path, "Coalesced_") && !(flags & (O_WRONLY | O_RDWR)) && !strstr(path, ".fps"))
            return c.ret(open_coalesced(path, flags));
        c.ret(open(path, flags, c.arg(2)));
    });
    IMPORT(read); IMPORT(write); IMPORT(close); IMPORT(lseek); IMPORT(access); IMPORT(chdir); IMPORT(chmod);
    IMPORT(mkdir); IMPORT(rmdir); IMPORT(remove); IMPORT(rename); IMPORT(unlink);
    register_import("fcntl", [](Ctx& c) { c.ret(fcntl(c.arg(0), c.arg(1), static_cast<long>(c.arg(2)))); });
    register_import("ioctl", [](Ctx& c) { c.ret(ioctl(c.arg(0), c.arg(1), mem::h(c.arg(2)))); });
    register_import("stat", [](Ctx& c) {
        struct stat s;
        int r = stat(mem::h<char>(c.arg(0)), &s);
        if (!r) stat_to_guest(s, c.arg(1));
        c.ret(r);
    });
    register_import("fstat", [](Ctx& c) {
        struct stat s;
        int r = fstat(c.arg(0), &s);
        if (!r) stat_to_guest(s, c.arg(1));
        c.ret(r);
    });
    register_import("utimes", [](Ctx& c) {
        timeval tv[2];
        if (c.arg(1)) { tv[0] = tv_from_guest(c.arg(1)); tv[1] = tv_from_guest(c.arg(1) + 8); }
        c.ret(utimes(mem::h<char>(c.arg(0)), c.arg(1) ? tv : nullptr));
    });
    register_import("opendir", [](Ctx& c) {
        DIR* d = opendir(mem::h<char>(c.arg(0)));
        if (!d) return c.ret(0);
        u32 g = mem::g(mem::calloc(1, 16));
        u32 ent = mem::g(mem::calloc(1, sizeof(dirent)));  // bionic dirent is the same on 32 and 64 bit
        std::lock_guard lk(files_lock);
        dirs[g] = d;
        dirents[g] = ent;
        c.ret(g);
    });
    register_import("readdir", [](Ctx& c) {
        DIR* d;
        u32 ent;
        {
            std::lock_guard lk(files_lock);
            d = dirs.at(c.arg(0));
            ent = dirents.at(c.arg(0));
        }
        dirent* e = readdir(d);
        if (!e) return c.ret(0);
        memcpy(mem::h(ent), e, sizeof(dirent));
        c.ret(ent);
    });
    register_import("closedir", [](Ctx& c) {
        std::lock_guard lk(files_lock);
        int r = closedir(dirs.at(c.arg(0)));
        mem::free(mem::h(dirents.at(c.arg(0))));
        mem::free(mem::h(c.arg(0)));
        dirs.erase(c.arg(0));
        dirents.erase(c.arg(0));
        c.ret(r);
    });

    // --- stdio
    register_import("fopen", [](Ctx& c) {
        FILE* f = fopen(mem::h<char>(c.arg(0)), mem::h<char>(c.arg(1)));
        if (!f) return c.ret(0);
        u32 g = mem::g(mem::calloc(1, kFileSize));
        std::lock_guard lk(files_lock);
        files[g] = f;
        c.ret(g);
    });
    register_import("fclose", [](Ctx& c) {
        FILE* f = hfile(c.arg(0));
        if (c.arg(0) - g_sF < 3 * kFileSize) return c.ret(0);
        int r = fclose(f);
        std::lock_guard lk(files_lock);
        files.erase(c.arg(0));
        mem::free(mem::h(c.arg(0)));
        c.ret(r);
    });
    register_import("fread", [](Ctx& c) { c.ret(fread(mem::h(c.arg(0)), c.arg(1), c.arg(2), hfile(c.arg(3)))); });
    register_import("fwrite", [](Ctx& c) { c.ret(fwrite(mem::h(c.arg(0)), c.arg(1), c.arg(2), hfile(c.arg(3)))); });
    register_import("fseek", [](Ctx& c) { c.ret(fseek(hfile(c.arg(0)), static_cast<s32>(c.arg(1)), c.arg(2))); });
    register_import("ftell", [](Ctx& c) { c.ret(static_cast<u32>(ftell(hfile(c.arg(0))))); });
    register_import("rewind", [](Ctx& c) { rewind(hfile(c.arg(0))); });
    register_import("fflush", [](Ctx& c) { c.ret(fflush(c.arg(0) ? hfile(c.arg(0)) : nullptr)); });
    register_import("fgetc", [](Ctx& c) { c.ret(fgetc(hfile(c.arg(0)))); });
    register_import("fputc", [](Ctx& c) { c.ret(fputc(c.arg(0), hfile(c.arg(1)))); });
    register_import("fgets", [](Ctx& c) {
        char* r = fgets(mem::h<char>(c.arg(0)), c.arg(1), hfile(c.arg(2)));
        c.ret(r ? c.arg(0) : 0);
    });
    register_import("putchar", [](Ctx& c) { c.ret(fputc(c.arg(0), hfile(g_sF + kFileSize))); });
    register_import("puts", [](Ctx& c) { LOGI("guest: %s", mem::h<char>(c.arg(0))); c.ret(1); });

    register_import("printf", [](Ctx& c) {
        ArgReader ar{c, 1};
        std::string s = gformat(mem::h<char>(c.arg(0)), {&ar});
        c.ret(fputs(s.c_str(), hfile(g_sF + kFileSize)) >= 0 ? s.size() : -1);
    });
    register_import("vprintf", [](Ctx& c) {
        std::string s = gformat(mem::h<char>(c.arg(0)), {nullptr, c.arg(1)});
        c.ret(fputs(s.c_str(), hfile(g_sF + kFileSize)) >= 0 ? s.size() : -1);
    });
    register_import("fprintf", [](Ctx& c) {
        ArgReader ar{c, 2};
        std::string s = gformat(mem::h<char>(c.arg(1)), {&ar});
        c.ret(fputs(s.c_str(), hfile(c.arg(0))) >= 0 ? s.size() : -1);
    });
    register_import("sprintf", [](Ctx& c) {
        ArgReader ar{c, 2};
        std::string s = gformat(mem::h<char>(c.arg(1)), {&ar});
        memcpy(mem::h(c.arg(0)), s.c_str(), s.size() + 1);
        c.ret(s.size());
    });
    register_import("vsprintf", [](Ctx& c) {
        std::string s = gformat(mem::h<char>(c.arg(1)), {nullptr, c.arg(2)});
        memcpy(mem::h(c.arg(0)), s.c_str(), s.size() + 1);
        c.ret(s.size());
    });
    static auto put_n = [](Ctx& c, const std::string& s) {
        u32 n = c.arg(1);
        if (n) {
            size_t k = std::min<size_t>(s.size(), n - 1);
            memcpy(mem::h(c.arg(0)), s.data(), k);
            mem::h<char>(c.arg(0))[k] = 0;
        }
        c.ret(s.size());
    };
    register_import("snprintf", [](Ctx& c) {
        ArgReader ar{c, 3};
        put_n(c, gformat(mem::h<char>(c.arg(2)), {&ar}));
    });
    register_import("vsnprintf", [](Ctx& c) { put_n(c, gformat(mem::h<char>(c.arg(2)), {nullptr, c.arg(3)})); });
    register_import("__android_log_print", [](Ctx& c) {
        ArgReader ar{c, 3};
        std::string s = gformat(mem::h<char>(c.arg(2)), {&ar});
        __android_log_write(c.arg(0), c.arg(1) ? mem::h<char>(c.arg(1)) : "ddguest", s.c_str());
        c.ret(1);
    });
    register_import("sscanf", [](Ctx& c) {
        ArgReader ar{c, 2};
        c.ret(gscan(mem::h<char>(c.arg(0)), nullptr, mem::h<char>(c.arg(1)), {&ar}));
    });
    register_import("fscanf", [](Ctx& c) {
        ArgReader ar{c, 2};
        c.ret(gscan(nullptr, hfile(c.arg(0)), mem::h<char>(c.arg(1)), {&ar}));
    });
    register_import("vscanf", [](Ctx& c) { c.ret(EOF); });

    // --- sockets (sockaddr/fd_set layouts match; timeval/addrinfo/hostent don't)
    IMPORT(socket); IMPORT(bind); IMPORT(connect); IMPORT(listen); IMPORT(accept); IMPORT(send); IMPORT(recv);
    IMPORT(sendto); IMPORT(recvfrom); IMPORT(shutdown); IMPORT(getsockname); IMPORT(gethostname); IMPORT(inet_addr);
    register_import("setsockopt", [](Ctx& c) {
        if (c.arg(1) == SOL_SOCKET && (c.arg(2) == SO_RCVTIMEO || c.arg(2) == SO_SNDTIMEO)) {
            timeval tv = tv_from_guest(c.arg(3));
            return c.ret(setsockopt(c.arg(0), c.arg(1), c.arg(2), &tv, sizeof tv));
        }
        c.ret(setsockopt(c.arg(0), c.arg(1), c.arg(2), mem::h(c.arg(3)), c.arg(4)));
    });
    IMPORT(getsockopt);
    register_import("select", [](Ctx& c) {
        timeval tv, *ptv = nullptr;
        if (c.arg(4)) { tv = tv_from_guest(c.arg(4)); ptv = &tv; }
        int r = select(c.arg(0), mem::h<fd_set>(c.arg(1)), mem::h<fd_set>(c.arg(2)), mem::h<fd_set>(c.arg(3)), ptv);
        if (ptv) tv_to_guest(tv, c.arg(4));
        c.ret(r);
    });
    register_import("inet_ntoa", [](Ctx& c) {
        in_addr a{c.arg(0)};
        u32 b = per_thread_buf(32);
        strcpy(mem::h<char>(b), inet_ntoa(a));
        c.ret(b);
    });
    register_import("gethostbyname", [](Ctx& c) { c.ret(hostent_to_guest(gethostbyname(mem::h<char>(c.arg(0))))); });
    register_import("gethostbyaddr", [](Ctx& c) {
        c.ret(hostent_to_guest(gethostbyaddr(mem::h(c.arg(0)), c.arg(1), c.arg(2))));
    });
    register_import("getaddrinfo", [](Ctx& c) {
        addrinfo hints{}, *res = nullptr;
        if (c.arg(2)) {
            hints.ai_flags = mem::rd32(c.arg(2));
            hints.ai_family = mem::rd32(c.arg(2) + 4);
            hints.ai_socktype = mem::rd32(c.arg(2) + 8);
            hints.ai_protocol = mem::rd32(c.arg(2) + 12);
        }
        int r = getaddrinfo(c.arg(0) ? mem::h<char>(c.arg(0)) : nullptr, c.arg(1) ? mem::h<char>(c.arg(1)) : nullptr,
                            c.arg(2) ? &hints : nullptr, &res);
        mem::wr32(c.arg(3), r ? 0 : addrinfo_to_guest(res));
        if (res) freeaddrinfo(res);
        c.ret(r);
    });
    register_import("freeaddrinfo", [](Ctx& c) {
        for (u32 a = c.arg(0); a;) {
            u32 next = mem::rd32(a + 28);
            if (mem::rd32(a + 20)) mem::free(mem::h(mem::rd32(a + 20)));
            mem::free(mem::h(mem::rd32(a + 24)));
            mem::free(mem::h(a));
            a = next;
        }
    });
}
