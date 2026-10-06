// JNI in both directions:
//  - the guest gets a fake JavaVM/JNIEnv (tables of svc thunks) living in guest memory;
//    Java objects/IDs cross over as 32-bit handles.
//  - Java calls the guest's native methods through host trampolines registered with ART.
#include <dlfcn.h>
#include <jni.h>

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "core.h"

GuestLib g_lib;
bool register_guest_native(JNIEnv* e, jclass cls, const char* name, const char* sig, u32 fn);

namespace {
JavaVM* g_vm;
u32 g_guest_vm, g_guest_env;
thread_local JNIEnv* t_env;

JNIEnv* env() {
    if (!t_env && g_vm->GetEnv(reinterpret_cast<void**>(&t_env), JNI_VERSION_1_6) != JNI_OK)
        g_vm->AttachCurrentThread(&t_env, nullptr);
    return t_env;
}

// ---------------------------------------------------------------- object handles
std::mutex ref_lock;
std::vector<jobject> refs{nullptr};
std::vector<u32> free_ids;
thread_local std::vector<u32> t_locals;  // local handles created on this thread

u32 put(jobject o) {
    if (!o) return 0;
    std::lock_guard lk(ref_lock);
    u32 id;
    if (!free_ids.empty()) { id = free_ids.back(); free_ids.pop_back(); refs[id] = o; }
    else { id = refs.size(); refs.push_back(o); }
    return id;
}
u32 put_local(jobject o) {
    u32 id = put(o);
    if (id) t_locals.push_back(id);
    return id;
}
jobject get(u32 id) {
    if (!id) return nullptr;
    std::lock_guard lk(ref_lock);
    if (id >= refs.size()) fatal("bad guest jobject %u", id);
    return refs[id];
}
void drop(u32 id) {
    if (!id) return;
    std::lock_guard lk(ref_lock);
    refs[id] = nullptr;
    free_ids.push_back(id);
}
void forget_local(u32 id) {
    for (auto it = t_locals.rbegin(); it != t_locals.rend(); ++it)
        if (*it == id) { *it = 0; return; }
}
// Locals handed out during a Java->guest call die when it returns, like real JNI locals.
struct LocalFrame {
    size_t mark = t_locals.size();
    ~LocalFrame() {
        for (size_t i = mark; i < t_locals.size(); i++) drop(t_locals[i]);
        t_locals.resize(mark);
    }
};

// ---------------------------------------------------------------- method / field IDs
struct Member {
    void* id;
    std::string name;
    std::string args;  // one char per parameter: ZBCSIJFDL
    char ret;
};
std::mutex member_lock;
std::vector<Member> members{{nullptr, "", "", 'V'}};
std::unordered_map<void*, u32> member_ids;

void parse_sig(const char* s, std::string& args, char& ret) {
    args.clear();
    if (*s++ != '(') { ret = s[-1] == '[' ? 'L' : s[-1]; return; }  // field signature
    while (*s && *s != ')') {
        char c = *s;
        while (*s == '[') s++;
        if (*s == 'L') while (*s && *s != ';') s++;
        args += c == '[' ? 'L' : c;
        s++;
    }
    s++;
    ret = *s == '[' ? 'L' : *s;
}
u32 member(void* id, const char* name, const char* sig) {
    if (!id) return 0;
    std::lock_guard lk(member_lock);
    auto [it, fresh] = member_ids.try_emplace(id, members.size());
    if (fresh) {
        Member m{id, name, "", 'V'};
        parse_sig(sig, m.args, m.ret);
        members.push_back(m);
    }
    return it->second;
}
Member mget(u32 i) {
    std::lock_guard lk(member_lock);
    if (!i || i >= members.size()) fatal("bad guest method/field id %u", i);
    return members[i];
}

// ---------------------------------------------------------------- calls
std::vector<jvalue> read_args(const Member& m, VaReader va) {
    std::vector<jvalue> v(m.args.size());
    for (size_t i = 0; i < m.args.size(); i++) {
        switch (m.args[i]) {
            case 'J': v[i].j = static_cast<jlong>(va.dw()); break;
            case 'D': { u64 b = va.dw(); memcpy(&v[i].d, &b, 8); break; }
            case 'F': { u64 b = va.dw(); double d; memcpy(&d, &b, 8); v[i].f = static_cast<float>(d); break; }  // promoted
            case 'L': v[i].l = get(va.w()); break;
            case 'Z': v[i].z = static_cast<jboolean>(va.w()); break;
            case 'B': v[i].b = static_cast<jbyte>(va.w()); break;
            case 'C': v[i].c = static_cast<jchar>(va.w()); break;
            case 'S': v[i].s = static_cast<jshort>(va.w()); break;
            default: v[i].i = static_cast<jint>(va.w()); break;
        }
    }
    return v;
}
std::vector<jvalue> read_jvalues(const Member& m, u32 arr) {
    std::vector<jvalue> v(m.args.size());
    for (size_t i = 0; i < m.args.size(); i++) {
        u32 a = arr + 8 * i;
        if (m.args[i] == 'L') v[i].l = get(mem::rd32(a));
        else memcpy(&v[i], mem::h(a), 8);
    }
    return v;
}

// one line every 5 s: frames presented and guest->host calls per second
void count_frame() {
    static std::atomic<u32> frames{0};
    static std::atomic<int64_t> last{0};
    static u64 last_svc = 0;
    frames++;
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = ts.tv_sec, prev = last.load();
    if (now - prev >= 5 && last.compare_exchange_strong(prev, now)) {
        u64 svc = g_svc_count.load();
        if (prev) LOGI("perf: %.1f fps, %llu host calls/s", frames.exchange(0) / double(now - prev),
                       static_cast<unsigned long long>((svc - last_svc) / (now - prev)));
        else frames = 0;
        last_svc = svc;
    }
}

void exception_guard() {
    if (env()->ExceptionCheck()) {
        LOGW("Java exception during guest JNI call:");
        env()->ExceptionDescribe();
        env()->ExceptionClear();
    }
}

enum Kind { kVirtual, kNonvirtual, kStatic, kCtor };
void call_java(Ctx& c, Kind k, char ret, int mode /*0 = ..., 1 = V, 2 = A*/) {
    JNIEnv* e = env();
    int first = k == kNonvirtual ? 4 : 3;  // env, obj/clazz, [clazz,] methodID, args...
    jobject obj = get(c.arg(1));
    jclass cls = static_cast<jclass>(k == kNonvirtual ? get(c.arg(2)) : obj);
    Member m = mget(c.arg(first - 1));
    auto mid = static_cast<jmethodID>(m.id);
    if (m.name == "JavaCallback_swapBuffers") count_frame();
    std::vector<jvalue> a;
    if (mode == 2) a = read_jvalues(m, c.arg(first));
    else if (mode == 1) a = read_args(m, {nullptr, c.arg(first)});
    else { ArgReader ar{c, first}; a = read_args(m, {&ar}); }
    const jvalue* av = a.data();
    jvalue r{};
#define DISPATCH(T, field)                                                                            \
    r.field = k == kStatic ? e->CallStatic##T##MethodA(cls, mid, av)                                  \
            : k == kNonvirtual ? e->CallNonvirtual##T##MethodA(obj, cls, mid, av)                     \
                               : e->Call##T##MethodA(obj, mid, av)
    switch (ret) {
        case 'V':
            if (k == kStatic) e->CallStaticVoidMethodA(cls, mid, av);
            else if (k == kNonvirtual) e->CallNonvirtualVoidMethodA(obj, cls, mid, av);
            else e->CallVoidMethodA(obj, mid, av);
            break;
        case 'L':
            if (k == kCtor) r.l = e->NewObjectA(cls, mid, av);
            else DISPATCH(Object, l);
            break;
        case 'Z': DISPATCH(Boolean, z); break;
        case 'B': DISPATCH(Byte, b); break;
        case 'C': DISPATCH(Char, c); break;
        case 'S': DISPATCH(Short, s); break;
        case 'I': DISPATCH(Int, i); break;
        case 'J': DISPATCH(Long, j); break;
        case 'F': DISPATCH(Float, f); break;
        case 'D': DISPATCH(Double, d); break;
    }
#undef DISPATCH
    exception_guard();
    switch (ret) {
        case 'L': c.ret(put_local(r.l)); break;
        case 'Z': c.ret(r.z); break;
        case 'B': c.ret(static_cast<u32>(r.b)); break;
        case 'C': c.ret(r.c); break;
        case 'S': c.ret(static_cast<u32>(r.s)); break;
        case 'I': c.ret(r.i); break;
        case 'J': c.ret64(r.j); break;
        case 'F': write_ret(c, r.f); break;
        case 'D': write_ret(c, r.d); break;
    }
}

const char kTypes[] = "LZBCSIJFDV";  // JNI table order: Object Boolean Byte Char Short Int Long Float Double Void
const char kPrims[] = "ZBCSIJFD";
int prim_size(char t) { return t == 'J' || t == 'D' ? 8 : t == 'I' || t == 'F' ? 4 : t == 'C' || t == 'S' ? 2 : 1; }

// arrays handed out by Get*ArrayElements: guest buffer -> (global array ref, element type)
std::mutex arr_lock;
std::unordered_map<u32, std::pair<jarray, char>> pinned;

void region(JNIEnv* e, char t, jarray a, jsize start, jsize len, void* buf, bool set) {
#define R(T, J, CT)                                                                    \
    case J:                                                                            \
        if (set) e->Set##T##ArrayRegion(static_cast<CT##Array>(a), start, len, static_cast<CT*>(buf)); \
        else e->Get##T##ArrayRegion(static_cast<CT##Array>(a), start, len, static_cast<CT*>(buf));     \
        break;
    switch (t) {
        R(Boolean, 'Z', jboolean) R(Byte, 'B', jbyte) R(Char, 'C', jchar) R(Short, 'S', jshort)
        R(Int, 'I', jint) R(Long, 'J', jlong) R(Float, 'F', jfloat) R(Double, 'D', jdouble)
    }
#undef R
}
char array_type(jarray a) {
    JNIEnv* e = env();
    jclass cls = e->GetObjectClass(a);
    jmethodID getName = e->GetMethodID(e->FindClass("java/lang/Class"), "getName", "()Ljava/lang/String;");
    auto name = static_cast<jstring>(e->CallObjectMethod(cls, getName));
    const char* s = e->GetStringUTFChars(name, nullptr);
    char t = s[1];
    e->ReleaseStringUTFChars(name, s);
    return t;
}
u32 pin_array(jarray a, char t, u32 is_copy) {
    JNIEnv* e = env();
    jsize n = e->GetArrayLength(a);
    void* buf = mem::alloc(n * prim_size(t) + 1);
    region(e, t, a, 0, n, buf, false);
    if (is_copy) *mem::h<u8>(is_copy) = 1;
    std::lock_guard lk(arr_lock);
    pinned[mem::g(buf)] = {static_cast<jarray>(e->NewGlobalRef(a)), t};
    return mem::g(buf);
}
void unpin_array(u32 buf, u32 mode) {
    JNIEnv* e = env();
    std::pair<jarray, char> p;
    {
        std::lock_guard lk(arr_lock);
        p = pinned.at(buf);
    }
    if (mode != JNI_ABORT) region(e, p.second, p.first, 0, e->GetArrayLength(p.first), mem::h(buf), true);
    if (mode != JNI_COMMIT) {
        e->DeleteGlobalRef(p.first);
        mem::free(mem::h(buf));
        std::lock_guard lk(arr_lock);
        pinned.erase(buf);
    }
}

// ---------------------------------------------------------------- the guest JNIEnv table
void jni_fn(Ctx& c, int idx);
template <int I> void jni_thunk(Ctx& c) { jni_fn(c, I); }
template <int... I> constexpr std::array<Handler, sizeof...(I)> jni_handlers(std::integer_sequence<int, I...>) {
    return {&jni_thunk<I>...};
}

void jni_fn(Ctx& c, int idx) {
    JNIEnv* e = env();
    auto obj = [&](int i) { return get(c.arg(i)); };
    auto str = [&](int i) { return mem::h<char>(c.arg(i)); };

    if (idx >= 34 && idx <= 63) return call_java(c, kVirtual, kTypes[(idx - 34) / 3], (idx - 34) % 3);
    if (idx >= 64 && idx <= 93) return call_java(c, kNonvirtual, kTypes[(idx - 64) / 3], (idx - 64) % 3);
    if (idx >= 114 && idx <= 143) return call_java(c, kStatic, kTypes[(idx - 114) / 3], (idx - 114) % 3);
    if (idx >= 28 && idx <= 30) return call_java(c, kCtor, 'L', idx - 28);

    // fields: Get(env, obj, id) / Set(env, obj, id, value), statics take the class instead
    bool get_f = (idx >= 95 && idx <= 103) || (idx >= 145 && idx <= 153);
    bool set_f = (idx >= 104 && idx <= 112) || (idx >= 154 && idx <= 162);
    if (get_f || set_f) {
        bool is_static = idx >= 145;
        int base = idx >= 154 ? 154 : idx >= 145 ? 145 : idx >= 104 ? 104 : 95;
        char t = kTypes[idx - base];
        jobject o = obj(1);
        auto fid = static_cast<jfieldID>(mget(c.arg(2)).id);
        auto cls = static_cast<jclass>(o);
        if (get_f) {
            jvalue r{};
#define G(T, f) r.f = is_static ? e->GetStatic##T##Field(cls, fid) : e->Get##T##Field(o, fid)
            switch (t) {
                case 'L': G(Object, l); return c.ret(put_local(r.l));
                case 'Z': G(Boolean, z); return c.ret(r.z);
                case 'B': G(Byte, b); return c.ret(static_cast<u32>(r.b));
                case 'C': G(Char, c); return c.ret(r.c);
                case 'S': G(Short, s); return c.ret(static_cast<u32>(r.s));
                case 'I': G(Int, i); return c.ret(r.i);
                case 'J': G(Long, j); return c.ret64(r.j);
                case 'F': G(Float, f); return write_ret(c, r.f);
                case 'D': G(Double, d); return write_ret(c, r.d);
            }
#undef G
        }
        ArgReader ar{c, 3};
        jvalue v{};
        if (t == 'J' || t == 'D') { u64 b = ar.dw(); memcpy(&v, &b, 8); }
        else if (t == 'L') v.l = get(ar.w());
        else { u32 w = ar.w(); memcpy(&v, &w, 4); }
#define S(T, f) is_static ? e->SetStatic##T##Field(cls, fid, v.f) : e->Set##T##Field(o, fid, v.f)
        switch (t) {
            case 'L': S(Object, l); break;
            case 'Z': S(Boolean, z); break;
            case 'B': S(Byte, b); break;
            case 'C': S(Char, c); break;
            case 'S': S(Short, s); break;
            case 'I': S(Int, i); break;
            case 'J': S(Long, j); break;
            case 'F': S(Float, f); break;
            case 'D': S(Double, d); break;
        }
#undef S
        return;
    }
    if (idx >= 175 && idx <= 182) {
        char t = kPrims[idx - 175];
        jsize n = c.arg(1);
        jarray a = nullptr;
        switch (t) {
            case 'Z': a = e->NewBooleanArray(n); break;
            case 'B': a = e->NewByteArray(n); break;
            case 'C': a = e->NewCharArray(n); break;
            case 'S': a = e->NewShortArray(n); break;
            case 'I': a = e->NewIntArray(n); break;
            case 'J': a = e->NewLongArray(n); break;
            case 'F': a = e->NewFloatArray(n); break;
            case 'D': a = e->NewDoubleArray(n); break;
        }
        return c.ret(put_local(a));
    }
    if (idx >= 183 && idx <= 190) return c.ret(pin_array(static_cast<jarray>(obj(1)), kPrims[idx - 183], c.arg(2)));
    if (idx >= 191 && idx <= 198) return unpin_array(c.arg(2), c.arg(3));
    if (idx >= 199 && idx <= 214) {
        bool set = idx >= 207;
        region(e, kPrims[idx - (set ? 207 : 199)], static_cast<jarray>(obj(1)), c.arg(2), c.arg(3), mem::h(c.arg(4)), set);
        return;
    }

    switch (idx) {
        case 4: return c.ret(e->GetVersion());
        case 6: {
            jclass cls = e->FindClass(str(1));
            exception_guard();
            return c.ret(put_local(cls));
        }
        case 10: return c.ret(put_local(e->GetSuperclass(static_cast<jclass>(obj(1)))));
        case 11: return c.ret(e->IsAssignableFrom(static_cast<jclass>(obj(1)), static_cast<jclass>(obj(2))));
        case 13: return c.ret(e->Throw(static_cast<jthrowable>(obj(1))));
        case 14: return c.ret(e->ThrowNew(static_cast<jclass>(obj(1)), str(2)));
        case 15: return c.ret(put_local(e->ExceptionOccurred()));
        case 16: return e->ExceptionDescribe();
        case 17: return e->ExceptionClear();
        case 18: fatal("guest FatalError: %s", str(1));
        case 19: return c.ret(e->PushLocalFrame(c.arg(1)));
        case 20: return c.ret(put_local(e->PopLocalFrame(obj(1))));  // ponytail: old handles leak until frame exit
        case 21: return c.ret(put(e->NewGlobalRef(obj(1))));
        case 22: {
            e->DeleteGlobalRef(obj(1));
            return drop(c.arg(1));
        }
        case 23: {
            e->DeleteLocalRef(obj(1));
            forget_local(c.arg(1));
            return drop(c.arg(1));
        }
        case 24: return c.ret(e->IsSameObject(obj(1), obj(2)));
        case 25: return c.ret(put_local(e->NewLocalRef(obj(1))));
        case 26: return c.ret(e->EnsureLocalCapacity(c.arg(1)));
        case 27: return c.ret(put_local(e->AllocObject(static_cast<jclass>(obj(1)))));
        case 31: return c.ret(put_local(e->GetObjectClass(obj(1))));
        case 32: return c.ret(e->IsInstanceOf(obj(1), static_cast<jclass>(obj(2))));
        case 33: case 113: case 94: case 144: {
            auto cls = static_cast<jclass>(obj(1));
            void* id = idx == 33    ? static_cast<void*>(e->GetMethodID(cls, str(2), str(3)))
                       : idx == 113 ? static_cast<void*>(e->GetStaticMethodID(cls, str(2), str(3)))
                       : idx == 94  ? static_cast<void*>(e->GetFieldID(cls, str(2), str(3)))
                                    : static_cast<void*>(e->GetStaticFieldID(cls, str(2), str(3)));
            if (!id) LOGW("guest asked for missing member %s %s", str(2), str(3));
            exception_guard();
            return c.ret(member(id, str(2), str(3)));
        }
        case 163: return c.ret(put_local(e->NewString(mem::h<jchar>(c.arg(1)), c.arg(2))));
        case 164: return c.ret(e->GetStringLength(static_cast<jstring>(obj(1))));
        case 165: case 224: {
            auto s = static_cast<jstring>(obj(1));
            jsize n = e->GetStringLength(s);
            auto* buf = static_cast<jchar*>(mem::alloc((n + 1) * 2));
            e->GetStringRegion(s, 0, n, buf);
            buf[n] = 0;
            if (c.arg(2)) *mem::h<u8>(c.arg(2)) = 1;
            return c.ret(mem::g(buf));
        }
        case 166: case 225: return mem::free(mem::h(c.arg(2)));
        case 167: return c.ret(put_local(e->NewStringUTF(str(1))));
        case 168: return c.ret(e->GetStringUTFLength(static_cast<jstring>(obj(1))));
        case 169: {
            if (!c.arg(1)) return c.ret(0);
            const char* s = e->GetStringUTFChars(static_cast<jstring>(obj(1)), nullptr);
            u32 g = mem::strdup(s);
            e->ReleaseStringUTFChars(static_cast<jstring>(obj(1)), s);
            if (c.arg(2)) *mem::h<u8>(c.arg(2)) = 1;
            return c.ret(g);
        }
        case 170: return mem::free(mem::h(c.arg(2)));
        case 171: return c.ret(e->GetArrayLength(static_cast<jarray>(obj(1))));
        case 172: return c.ret(put_local(e->NewObjectArray(c.arg(1), static_cast<jclass>(obj(2)), obj(3))));
        case 173: return c.ret(put_local(e->GetObjectArrayElement(static_cast<jobjectArray>(obj(1)), c.arg(2))));
        case 174: return e->SetObjectArrayElement(static_cast<jobjectArray>(obj(1)), c.arg(2), obj(3));
        case 215: {
            auto cls = static_cast<jclass>(obj(1));
            bool ok = true;
            for (u32 i = 0; i < c.arg(3); i++) {
                u32 m = c.arg(2) + 12 * i;
                ok &= register_guest_native(e, cls, mem::h<char>(mem::rd32(m)), mem::h<char>(mem::rd32(m + 4)),
                                            mem::rd32(m + 8));
            }
            return c.ret(ok ? 0 : -1);
        }
        case 216: return c.ret(e->UnregisterNatives(static_cast<jclass>(obj(1))));
        case 217: return c.ret(e->MonitorEnter(obj(1)));
        case 218: return c.ret(e->MonitorExit(obj(1)));
        case 219:
            mem::wr32(c.arg(1), g_guest_vm);
            return c.ret(0);
        case 220: return e->GetStringRegion(static_cast<jstring>(obj(1)), c.arg(2), c.arg(3), mem::h<jchar>(c.arg(4)));
        case 221: return e->GetStringUTFRegion(static_cast<jstring>(obj(1)), c.arg(2), c.arg(3), str(4));
        case 222: {
            auto a = static_cast<jarray>(obj(1));
            return c.ret(pin_array(a, array_type(a), c.arg(2)));
        }
        case 223: return unpin_array(c.arg(2), c.arg(3));
        case 226: return c.ret(put(e->NewWeakGlobalRef(obj(1))));
        case 227: {
            e->DeleteWeakGlobalRef(obj(1));
            return drop(c.arg(1));
        }
        case 228: return c.ret(e->ExceptionCheck());
        case 229: {
            ArgReader ar{c, 2};
            return c.ret(put_local(e->NewDirectByteBuffer(mem::h(c.arg(1)), static_cast<jlong>(ar.dw()))));
        }
        case 230: return c.ret(mem::g(e->GetDirectBufferAddress(obj(1))));
        case 231: return c.ret64(e->GetDirectBufferCapacity(obj(1)));
        case 232: return c.ret(e->GetObjectRefType(obj(1)));
    }
    fatal("guest used unimplemented JNI function #%d", idx);
}

// ---------------------------------------------------------------- the guest JavaVM table
void vm_fn(Ctx& c, int idx) {
    switch (idx) {
        case 4: case 7: {  // AttachCurrentThread(AsDaemon)
            JNIEnv* e;
            int r = idx == 4 ? g_vm->AttachCurrentThread(&e, nullptr) : g_vm->AttachCurrentThreadAsDaemon(&e, nullptr);
            t_env = e;
            mem::wr32(c.arg(1), g_guest_env);
            return c.ret(r);
        }
        case 5: {
            int r = g_vm->DetachCurrentThread();
            t_env = nullptr;
            return c.ret(r);
        }
        case 6: {  // GetEnv
            JNIEnv* e;
            int r = g_vm->GetEnv(reinterpret_cast<void**>(&e), c.arg(2));
            if (r == JNI_OK) {
                t_env = e;
                mem::wr32(c.arg(1), g_guest_env);
            }
            return c.ret(r);
        }
    }
    fatal("guest used unimplemented JavaVM function #%d", idx);
}
template <int I> void vm_thunk(Ctx& c) { vm_fn(c, I); }

u32 build_table(const Handler* h, int n, const char* what) {
    u32 table = mem::g(mem::calloc(n, 4));
    for (int i = 0; i < n; i++) mem::wr32(table + 4 * i, make_thunk(h[i], what, true));
    u32 obj = mem::g(mem::calloc(1, 4));
    mem::wr32(obj, table);
    return obj;
}

// ---------------------------------------------------------------- Java -> guest natives
struct Native {
    const char* name;
    const char* sig;
    void* host;
    u32 guest = 0;
};
extern Native natives[];

struct Words {
    std::vector<u32> w;
    void add(u32 v) { w.push_back(v); }
    void add64(u64 v) {
        if (w.size() & 1) w.push_back(0);
        w.push_back(static_cast<u32>(v));
        w.push_back(static_cast<u32>(v >> 32));
    }
};
template <typename T> void push_arg(Words& w, T v) {
    if constexpr (std::is_same_v<T, jfloat>) { u32 b; memcpy(&b, &v, 4); w.add(b); }
    else if constexpr (std::is_same_v<T, jlong>) w.add64(static_cast<u64>(v));
    else if constexpr (std::is_same_v<T, jdouble>) { u64 b; memcpy(&b, &v, 8); w.add64(b); }
    else if constexpr (std::is_pointer_v<T>) w.add(put_local(v));
    else w.add(static_cast<u32>(v));
}

template <int N, typename R, typename... A>
R tramp(JNIEnv* e, jobject thiz, A... a) {
    t_env = e;
    LocalFrame frame;
    Words w;
    w.add(g_guest_env);
    w.add(put_local(thiz));
    (push_arg(w, a), ...);
    u64 r = call_guest(natives[N].guest, w.w.data(), w.w.size());
    if constexpr (std::is_same_v<R, void>) return;
    else if constexpr (std::is_same_v<R, jboolean>) return static_cast<jboolean>(r & 0xff);
    else if constexpr (std::is_pointer_v<R>) return static_cast<R>(e->NewLocalRef(get(static_cast<u32>(r))));
    else return static_cast<R>(r);
}

#define N(i, name, sig, R, ...) {name, sig, reinterpret_cast<void*>(&tramp<i, R, ##__VA_ARGS__>)}
Native natives[] = {
    N(0, "AxisEvent", "(FFI)V", void, jfloat, jfloat, jint),
    N(1, "GPUStateChanged", "(Z)V", void, jboolean),
    N(2, "GetGraphicsPath", "()Ljava/lang/String;", jstring),
    N(3, "InteruptionChanged", "(Z)Z", jboolean, jboolean),
    N(4, "KeyPadChange", "(Z)V", void, jboolean),
    N(5, "LanguageSet", "(Ljava/lang/String;)V", void, jstring),
    N(6, "NetworkUpdate", "(ZZ)V", void, jboolean, jboolean),
    N(7, "Post_Init_Update", "(II)V", void, jint, jint),
    N(8, "SystemStats", "(J)Z", jboolean, jlong),
    N(9, "cleanup", "()V", void),
    N(10, "contentPurchased", "(Ljava/lang/String;)V", void, jstring),
    N(11, "init", "(IIFZLjava/lang/Object;ZLjava/lang/String;)Z", jboolean, jint, jint, jfloat, jboolean, jobject,
      jboolean, jstring),
    N(12, "initEGLCallback", "()Z", jboolean),
    N(13, "inputEvent", "(IIII)Z", jboolean, jint, jint, jint, jint),
    N(14, "keyEvent", "(IIILandroid/view/KeyEvent;)Z", jboolean, jint, jint, jint, jobject),
    N(15, "keyboardFinished", "(Ljava/lang/String;)V", void, jstring),
    N(16, "movieFinished", "()V", void),
    N(17, "render", "(II)V", void, jint, jint),
};
#undef N
}  // namespace

bool register_guest_native(JNIEnv* e, jclass cls, const char* name, const char* sig, u32 fn) {
    for (auto& n : natives) {
        if (strcmp(n.name, name) || strcmp(n.sig, sig)) continue;
        n.guest = fn;
        JNINativeMethod m{name, sig, n.host};
        return e->RegisterNatives(cls, &m, 1) == 0;
    }
    LOGE("guest registers unknown native %s%s", name, sig);
    return false;
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    env();
    mem::init();
    init_libc();
    init_gl();

    static constexpr auto jni = jni_handlers(std::make_integer_sequence<int, 233>{});
    g_guest_env = build_table(jni.data(), jni.size(), "JNIEnv");
    static constexpr Handler vmh[8] = {nullptr, nullptr, nullptr, nullptr, &vm_thunk<4>, &vm_thunk<5>, &vm_thunk<6>, &vm_thunk<7>};
    g_guest_vm = build_table(vmh, 8, "JavaVM");

    Dl_info info;
    dladdr(reinterpret_cast<void*>(&JNI_OnLoad), &info);
    std::string path = info.dli_fname;
    path = path.substr(0, path.rfind('/') + 1) + "libDunDefGuest.so";
    g_lib = load_guest(path.c_str());
    run_guest_constructors(g_lib);

    u32 onload = g_lib.sym("JNI_OnLoad");
    if (!onload) fatal("guest has no JNI_OnLoad");
    LocalFrame frame;
    jint r = static_cast<jint>(call_guest(onload, {g_guest_vm, 0}));
    LOGI("guest JNI_OnLoad returned %x", r);
    return r;
}
