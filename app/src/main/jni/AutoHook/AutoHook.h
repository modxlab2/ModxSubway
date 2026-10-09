#pragma once

#include "Struct.h"
#include <dlfcn.h>
#include <unistd.h>

typedef Il2CppClass1 *(*class_from_name_t)(const Il2CppImage *assembly, const char *name_space,
                                          const char *name);

typedef MethodInfo1 *(*class_get_method_from_name_t)(Il2CppClass1 *klass, const char *name,
                                                    int paramcount);

typedef Il2CppDomain *(*domain_get_t)();

typedef const Il2CppAssembly **(*domain_get_assemblies_t)(const Il2CppDomain *domain, size_t *size);

typedef const Il2CppImage *(*assembly_get_image_t)(const Il2CppAssembly *assembly);

typedef const Il2CppAssembly *(*domain_assembly_open_t)(Il2CppDomain *domain, const char *name);

typedef FieldInfo1 *(*class_get_field_from_name_t)(Il2CppClass1 *klass, const char *name);


// ================================================================
//   Retry / timeout constants  (∞ loop এড়ানোর জন্য)
// ================================================================
static constexpr int  kLoadClassMaxTries    = 8;      // total attempts
static constexpr int  kLoadClassSleepUs     = 200000; // 200 ms
static constexpr int  kDlopenRetryMax       = 200;    // libil2cpp waiting


template<typename T>
class Field : FieldInfo1 {
    bool statik;

    bool CheckStatic() {
        if ((type->attrs & 0x10) == 0)
            return false;

        if ((type->attrs & 0x40) != 0)
            return false;

        if (thread_static = offset == -1)
            LOGI(OBFUSCATE("thread static fields is not supported!"));

        return true;
    }

    bool CheckStaticFieldInfo(FieldInfo1 *fieldInfo) {
        if ((fieldInfo->type->attrs & 0x10) == 0)
            return false;

        if ((fieldInfo->type->attrs & 0x40) != 0)
            return false;

        if (fieldInfo->offset == -1)
            LOGI(OBFUSCATE("thread static fields is not supported!"));

        return true;
    }

public:
    bool init;
    bool thread_static;
    void *clazz;

    Field(FieldInfo1 *thiz, void *clas = NULL) {
        if (!CheckStaticFieldInfo(thiz))
            clazz = clas;
        init = (thiz != NULL);
        if (init) {
            parent = thiz->parent;
            offset = thiz->offset;
            name = thiz->name;
            token = thiz->token;
            type = thiz->type;
            statik = CheckStatic();
        }
    }

    DWORD get_offset() {
        return offset;
    }

    T get() {
        if (!init || thread_static) return T();
        if (statik) {
            return *(T *) ((uint64_t) parent->static_fields + offset);
        }
        return *(T *) ((uint64_t) clazz + offset);
    }

    void set(T val) {
        if (!init || thread_static) return;
        if (statik) {
            *(T *) ((uint64_t) parent->static_fields + offset) = val;
            return;
        }
        *(T *) ((uint64_t) clazz + offset) = val;
    }

    operator T() {
        return get();
    }

    void operator=(T val) {
        set(val);
    }

    bool operator==(Field other) {
        if (!init) return false;
        return (type == other.type && parent == other.parent &&
                offset == other.offset && name == other.name && token == other.token);
    }

    T operator()() {
        return get();
    }
};


class LoadClass {
    void *get_il2cpp() {
        void *mod = 0;
        int tries = 0;
        while (!mod && tries < kDlopenRetryMax) {
            mod = dlopen(OBFUSCATE("libil2cpp.so"), 4);
            if (!mod) { usleep(100 * 1000); tries++; }
        }
        return mod;
    }

    Il2CppClass1 *GetClassFromName(const char *name_space, const char *type_name) {
        void *h = get_il2cpp();
        if (!h) return nullptr;

        auto domain_get = (domain_get_t) dlsym(h, OBFUSCATE("il2cpp_domain_get"));
        if (!domain_get) return nullptr;

        auto dom = domain_get();
        if (!dom) return nullptr;

        size_t assemb_count = 0;
        auto domain_get_assemblies = (domain_get_assemblies_t)
                dlsym(h, OBFUSCATE("il2cpp_domain_get_assemblies"));
        if (!domain_get_assemblies) return nullptr;

        const Il2CppAssembly **allAssemb = domain_get_assemblies(dom, &assemb_count);
        if (!allAssemb) return nullptr;

        auto assembly_get_image = (assembly_get_image_t)
                dlsym(h, OBFUSCATE("il2cpp_assembly_get_image"));
        auto class_from_name = (class_from_name_t)
                dlsym(h, OBFUSCATE("il2cpp_class_from_name"));
        if (!assembly_get_image || !class_from_name) return nullptr;

        for (size_t i = 0; i < assemb_count; i++) {
            auto assemb = allAssemb[i];
            if (!assemb) continue;
            auto img = assembly_get_image(assemb);
            if (!img) continue;
            auto klass = class_from_name(img, name_space, type_name);
            if (klass) {
                namespaze_txt = name_space;
                clazz_txt     = type_name;
                dllname_txt   = img->name;
                dll           = img;
                return klass;
            }
        }
        return nullptr;
    }

public:
    const Il2CppImage *dll;
    Il2CppClass1 *thisclass;
    const char *namespaze_txt;
    const char *clazz_txt;
    const char *dllname_txt;

    // ------------------------------------------------------------
    //   ★ FIXED: infinite loop → bounded retry with sleep
    // ------------------------------------------------------------
    LoadClass(const char *namespaze, const char *clazz) {
        thisclass = nullptr;
        int tries = 0;
        do {
            thisclass = GetClassFromName(namespaze, clazz);
            if (thisclass) return;
            if (++tries >= kLoadClassMaxTries) return;   // ← গেমের obfuscation হলে
            usleep(kLoadClassSleepUs);                   //   menu freeze হবে না
        } while (!thisclass);
    }

    LoadClass(const char *namespaze, const char *clazz, const char *dllname) {
        void *h = get_il2cpp();
        if (!h) return;

        auto domain_assembly_open = (domain_assembly_open_t)
                dlsym(h, OBFUSCATE("il2cpp_domain_assembly_open"));
        auto assembly_get_image = (assembly_get_image_t)
                dlsym(h, OBFUSCATE("il2cpp_assembly_get_image"));
        auto domain_get = (domain_get_t)
                dlsym(h, OBFUSCATE("il2cpp_domain_get"));
        auto class_from_name = (class_from_name_t)
                dlsym(h, OBFUSCATE("il2cpp_class_from_name"));
        if (!domain_assembly_open || !assembly_get_image ||
            !domain_get || !class_from_name) return;

        auto dom = domain_get();
        if (!dom) return;

        auto asm_ = domain_assembly_open(dom, dllname);
        if (!asm_) return;

        dll = assembly_get_image(asm_);
        if (!dll) return;

        thisclass = nullptr;
        int tries = 0;
        do {
            thisclass = class_from_name(dll, namespaze, clazz);
            if (thisclass) return;
            if (++tries >= kLoadClassMaxTries) return;
            usleep(kLoadClassSleepUs);
        } while (!thisclass);
    }

    FieldInfo1 *GetFieldInfoByName(const char *name) {
        void *h = get_il2cpp();
        if (!h) return nullptr;
        auto class_get_field_from_name = (class_get_field_from_name_t)
                dlsym(h, OBFUSCATE("il2cpp_class_get_field_from_name"));
        if (!class_get_field_from_name) return nullptr;
        return class_get_field_from_name(thisclass, name);
    }

    template<typename T>
    Field<T> GetFieldByName(const char *name) {
        return Field<T>(GetFieldInfoByName(name), (void *) thisclass);
    }

    DWORD GetFieldOffset(const char *name) {
        FieldInfo1 *f = GetFieldInfoByName(name);
        return f ? f->offset : 0;
    }

    DWORD GetFieldOffset(FieldInfo1 *filed) {
        return filed ? filed->offset : 0;
    }

    MethodInfo1 *GetMethodInfoByName(const char *name, int paramcount) {
        void *h = get_il2cpp();
        if (!h || !thisclass) return nullptr;
        auto class_get_method_from_name = (class_get_method_from_name_t)
                dlsym(h, OBFUSCATE("il2cpp_class_get_method_from_name"));
        if (!class_get_method_from_name) return nullptr;
        return class_get_method_from_name(thisclass, name, paramcount);
    }

    DWORD GetMethodOffsetByName(const char *name, int paramcount) {
        auto res = GetMethodInfoByName(name, paramcount);
        if (res && res->methodPointer)
            return (DWORD) res->methodPointer;
        return 0;
    }
};


// ================================================================
//   icall resolver
// ================================================================
void *get_Method(const char *str) {
    void *(*il2cpp_resolve_icall_0)(const char *str) = nullptr;
    void *h = nullptr;
    int tries = 0;
    while (!h && tries < kDlopenRetryMax) {
        h = dlopen(OBFUSCATE("libil2cpp.so"), 4);
        if (!h) { usleep(100 * 1000); tries++; }
    }
    if (!h) return nullptr;

    do {
        il2cpp_resolve_icall_0 = (void *(*)(const char *))
                dlsym(h, OBFUSCATE("il2cpp_resolve_icall"));
        if (!il2cpp_resolve_icall_0) usleep(50 * 1000);
    } while (!il2cpp_resolve_icall_0);

    return il2cpp_resolve_icall_0(str);
}

#define InitResolveFunc(x, y) *reinterpret_cast<void **>(&x) = get_Method(y)
#define InitFunc(x, y)        if (y != 0) *reinterpret_cast<void **>(&x) = (void *)(y)
#define FieldBN(myfield, type, inst, nameSpacec, clazzz, fieldName, key) \
        Field<type> myfield = (new LoadClass(OBFUSCATE_KEY(nameSpacec, key), \
                                             OBFUSCATE_KEY(clazzz, key))) \
                             ->GetFieldByName<type>(OBFUSCATE_KEY(fieldName, key)); \
        myfield.clazz = inst