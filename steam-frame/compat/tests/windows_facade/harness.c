/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef SIZE_T (WINAPI *facade_virtual_query_fn)(LPCVOID, PMEMORY_BASIC_INFORMATION, SIZE_T);
typedef BOOL (WINAPI *facade_virtual_protect_fn)(LPVOID, SIZE_T, DWORD, PDWORD);
typedef uint32_t (WINAPI *engine_code_fn)(uint32_t);
typedef volatile uint32_t *engine_data_ptr;

static int failures;
static int cases;

static void check(int condition, const char *name) {
    ++cases;
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}

static int query_protect(LPCVOID address, DWORD *protect, MEMORY_BASIC_INFORMATION *out) {
    MEMORY_BASIC_INFORMATION local;
    if (VirtualQuery(address, &local, sizeof(local)) != sizeof(local)) return 0;
    if (protect) *protect = local.Protect;
    if (out) *out = local;
    return 1;
}

static int same_mapping_except_protect(const MEMORY_BASIC_INFORMATION *a,
                                       const MEMORY_BASIC_INFORMATION *b) {
    return a->BaseAddress == b->BaseAddress && a->AllocationBase == b->AllocationBase &&
           a->AllocationProtect == b->AllocationProtect && a->RegionSize == b->RegionSize &&
           a->State == b->State && a->Type == b->Type;
}

static int resolve_function(HMODULE module, const char *name, void *destination, SIZE_T size) {
    FARPROC raw = GetProcAddress(module, name);
    if (!raw || size != sizeof(raw)) return 0;
    memcpy(destination, &raw, sizeof(raw));
    return 1;
}

static int resolve_data(HMODULE module, const char *name, void *destination, SIZE_T size) {
    FARPROC raw = GetProcAddress(module, name);
    if (!raw || size != sizeof(raw)) return 0;
    memcpy(destination, &raw, sizeof(raw));
    return 1;
}

static LPVOID last_partial_executable_page(HMODULE module, DWORD page_size) {
    BYTE *base = (BYTE *)module;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return NULL;
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC)
        return NULL;
    DWORD alignment = nt->OptionalHeader.SectionAlignment;
    DWORD image_size = nt->OptionalHeader.SizeOfImage;
    if (!alignment || (alignment & (alignment - 1)) || alignment < page_size) return NULL;
    IMAGE_SECTION_HEADER *sections = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        DWORD va = sections[i].VirtualAddress;
        DWORD length = sections[i].Misc.VirtualSize;
        if (!length || !(sections[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) ||
            va >= image_size || length > image_size - va) continue;
        DWORD raw_end = va + length;
        DWORD mask = alignment - 1;
        if (raw_end > UINT32_MAX - mask) continue;
        DWORD mapped_end = (raw_end + mask) & ~mask;
        DWORD next = image_size;
        for (unsigned j = 0; j < nt->FileHeader.NumberOfSections; ++j) {
            DWORD candidate = sections[j].VirtualAddress;
            if (candidate > va && candidate < next) next = candidate;
        }
        if (mapped_end > image_size) mapped_end = image_size;
        if (mapped_end > next) mapped_end = next;
        if (mapped_end > raw_end && mapped_end - va >= page_size)
            return base + mapped_end - page_size;
    }
    return NULL;
}

static DWORD executable_form(DWORD protect) {
    DWORD modifiers = protect & ~0xffu;
    switch (protect & 0xffu) {
        case PAGE_READONLY: return modifiers | PAGE_EXECUTE_READ;
        case PAGE_READWRITE: return modifiers | PAGE_EXECUTE_READWRITE;
        case PAGE_WRITECOPY: return modifiers | PAGE_EXECUTE_WRITECOPY;
        default: return protect;
    }
}

static void report_protection(const char *label, BOOL changed, DWORD requested, DWORD old_protect,
                              DWORD error, int query_ok, const MEMORY_BASIC_INFORMATION *mbi) {
    printf("DIAG %s changed=%d request=0x%08lx old=0x%08lx error=%lu query=%d",
           label, (int)changed, requested, old_protect, error, query_ok);
    if (query_ok)
        printf(" state=0x%08lx protect=0x%08lx type=0x%08lx base=%p region=0x%lx",
               mbi->State, mbi->Protect, mbi->Type, mbi->BaseAddress, (unsigned long)mbi->RegionSize);
    putchar('\n');
}

static int load_facade(facade_virtual_query_fn *query, facade_virtual_protect_fn *protect) {
    HMODULE dll = LoadLibraryA("mp2vrcmp.dll");
    if (!dll) {
        printf("FAIL load facade error=%lu\n", GetLastError());
        return 0;
    }
    if (!resolve_function(dll, "VirtualQuery", query, sizeof(*query)) ||
        !resolve_function(dll, "VirtualProtect", protect, sizeof(*protect))) {
        printf("FAIL facade exports error=%lu\n", GetLastError());
        return 0;
    }
    return 1;
}

static int run_outside(const char *path) {
    facade_virtual_query_fn wrapped_query;
    facade_virtual_protect_fn wrapped_protect;
    if (!load_facade(&wrapped_query, &wrapped_protect)) return 2;
    (void)wrapped_protect;
    HMODULE engine = LoadLibraryA(path);
    if (!engine) {
        printf("FAIL load outside engine error=%lu path=%s\n", GetLastError(), path);
        return 2;
    }
    engine_code_fn code = NULL;
    if (!resolve_function(engine, "facade_test_code", &code, sizeof(code))) return 2;
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)code, 1, PAGE_READONLY, &old)) {
        printf("FAIL protect outside code error=%lu\n", GetLastError());
        return 2;
    }
    DWORD real_protect = 0, facade_protect = 0;
    MEMORY_BASIC_INFORMATION real_mbi, facade_mbi;
    int real_ok = query_protect((LPCVOID)code, &real_protect, &real_mbi);
    SIZE_T facade_size = wrapped_query((LPCVOID)code, &facade_mbi, sizeof(facade_mbi));
    int facade_ok = facade_size == sizeof(facade_mbi);
    if (facade_ok) facade_protect = facade_mbi.Protect;
    check(real_ok && real_mbi.State == MEM_COMMIT && real_mbi.Type == MEM_IMAGE &&
          real_protect == PAGE_READONLY, "outside control starts committed read-only image code");
    check(facade_ok && facade_protect == PAGE_READONLY,
          "same-name module in another directory is not normalized");
    if (!VirtualProtect((LPVOID)code, 1, PAGE_EXECUTE_READ, &old)) {
        printf("FAIL restore outside code error=%lu\n", GetLastError());
        return 2;
    }
    printf("SUMMARY outside: %d/%d checks failed\n", failures, cases);
    return failures ? 1 : 0;
}

static int run_same_directory(void) {
    facade_virtual_query_fn wrapped_query;
    facade_virtual_protect_fn wrapped_protect;
    if (!load_facade(&wrapped_query, &wrapped_protect)) return 2;
    HMODULE engine = LoadLibraryA("X_ModesMFC.dll");
    HMODULE control = LoadLibraryA("control/X_ControlMFC.dll");
    if (!engine || !control) {
        printf("FAIL load engine/control error=%lu\n", GetLastError());
        return 2;
    }
    engine_code_fn code = NULL, control_code = NULL;
    engine_data_ptr data = NULL, control_data = NULL;
    int code_found = resolve_function(engine, "facade_test_code", &code, sizeof(code));
    int control_code_found = resolve_function(control, "facade_test_code", &control_code, sizeof(control_code));
    int data_found = resolve_data(engine, "facade_test_data", &data, sizeof(data));
    int control_data_found = resolve_data(control, "facade_test_data", &control_data, sizeof(control_data));
    check(code_found && control_code_found && data_found && control_data_found,
          "same-directory test and raw KERNEL32 control modules export code and data");
    if (!code || !control_code || !data || !control_data) return 2;

    DWORD code_original = 0, control_code_original = 0, old = 0;
    if (!VirtualProtect((LPVOID)code, 1, PAGE_READONLY, &code_original) ||
        !VirtualProtect((LPVOID)control_code, 1, PAGE_READONLY, &control_code_original)) {
        printf("FAIL establish read-only code controls error=%lu\n", GetLastError());
        return 2;
    }
    DWORD real_protect = 0;
    MEMORY_BASIC_INFORMATION before, normalized;
    int real_ok = query_protect((LPCVOID)code, &real_protect, &before);
    SIZE_T got = wrapped_query((LPCVOID)code, &normalized, sizeof(normalized));
    check(real_ok && before.State == MEM_COMMIT && before.Type == MEM_IMAGE &&
          real_protect == PAGE_READONLY && got == sizeof(normalized) &&
          normalized.Protect == PAGE_EXECUTE_READ,
          "committed executable MEM_IMAGE page maps read-only to execute-read");
    check(got == sizeof(normalized) && same_mapping_except_protect(&before, &normalized),
          "query normalization preserves every other MBI field");

    DWORD raw_code_old = 0, facade_code_old = 0;
    BOOL raw_code_changed = VirtualProtect((LPVOID)control_code, 1, PAGE_READWRITE, &raw_code_old);
    DWORD raw_code_error = GetLastError();
    MEMORY_BASIC_INFORMATION raw_code_mbi;
    int raw_code_query = query_protect((LPCVOID)control_code, NULL, &raw_code_mbi);
    BOOL facade_code_changed = wrapped_protect((LPVOID)code, 1, PAGE_READWRITE, &facade_code_old);
    DWORD facade_code_error = GetLastError();
    MEMORY_BASIC_INFORMATION facade_code_mbi;
    int facade_code_query = query_protect((LPCVOID)code, NULL, &facade_code_mbi);
    report_protection("raw-image-code", raw_code_changed, PAGE_READWRITE, raw_code_old,
                      raw_code_error, raw_code_query, &raw_code_mbi);
    report_protection("facade-image-code", facade_code_changed, PAGE_READWRITE, facade_code_old,
                      facade_code_error, facade_code_query, &facade_code_mbi);
    check(raw_code_changed && facade_code_changed && raw_code_query && facade_code_query &&
          raw_code_old == PAGE_READONLY && facade_code_old == raw_code_old &&
          facade_code_mbi.Protect == executable_form(raw_code_mbi.Protect),
          "VirtualProtect executable result matches raw image mapping plus execute permission");
    if (!VirtualProtect((LPVOID)code, 1, code_original, &old) ||
        !VirtualProtect((LPVOID)control_code, 1, control_code_original, &old)) {
        printf("FAIL restore code controls error=%lu\n", GetLastError());
        return 2;
    }

    SYSTEM_INFO page_info;
    GetSystemInfo(&page_info);
    LPVOID last_partial_page = last_partial_executable_page(engine, page_info.dwPageSize);
    check(last_partial_page != NULL, "test engine has an executable section with a partial final page");
    if (!last_partial_page) return 2;
    if (!VirtualProtect(last_partial_page, page_info.dwPageSize, PAGE_READONLY, &old)) {
        printf("FAIL protect final partial code page error=%lu\n", GetLastError());
        return 2;
    }
    DWORD partial_real = 0;
    MEMORY_BASIC_INFORMATION partial_facade;
    int partial_query_ok = query_protect(last_partial_page, &partial_real, NULL);
    SIZE_T partial_got = wrapped_query(last_partial_page, &partial_facade, sizeof(partial_facade));
    check(partial_query_ok && partial_real == PAGE_READONLY && partial_got == sizeof(partial_facade) &&
          partial_facade.Protect == PAGE_EXECUTE_READ,
          "page-rounded executable extent includes the final partial VirtualSize page");
    if (!VirtualProtect(last_partial_page, page_info.dwPageSize, code_original, &old)) {
        printf("FAIL restore final partial code page error=%lu\n", GetLastError());
        return 2;
    }

    DWORD data_original = 0, control_data_original = 0;
    if (!VirtualProtect((LPVOID)data, sizeof(*data), PAGE_READONLY, &data_original) ||
        !VirtualProtect((LPVOID)control_data, sizeof(*control_data), PAGE_READONLY, &control_data_original)) {
        printf("FAIL protect ordinary data controls error=%lu\n", GetLastError());
        return 2;
    }
    DWORD data_readonly = 0;
    MEMORY_BASIC_INFORMATION data_before, data_after;
    int data_real_query = query_protect((LPCVOID)data, &data_readonly, &data_before);
    SIZE_T data_facade_size = wrapped_query((LPCVOID)data, &data_after, sizeof(data_after));
    check(data_real_query && data_before.Type == MEM_IMAGE && data_readonly == PAGE_READONLY &&
          data_facade_size == sizeof(data_after) && data_after.Protect == PAGE_READONLY &&
          same_mapping_except_protect(&data_before, &data_after),
          "read-only ordinary image data stays non-executable and preserves other query fields");
    DWORD raw_data_old = 0, facade_data_old = 0;
    BOOL raw_data_changed = VirtualProtect((LPVOID)control_data, sizeof(*control_data),
                                           PAGE_READWRITE, &raw_data_old);
    DWORD raw_data_error = GetLastError();
    MEMORY_BASIC_INFORMATION raw_data_mbi;
    int raw_data_query = query_protect((LPCVOID)control_data, NULL, &raw_data_mbi);
    BOOL facade_data_changed = wrapped_protect((LPVOID)data, sizeof(*data),
                                               PAGE_READWRITE, &facade_data_old);
    DWORD facade_data_error = GetLastError();
    MEMORY_BASIC_INFORMATION facade_data_mbi;
    int facade_data_query = query_protect((LPCVOID)data, NULL, &facade_data_mbi);
    report_protection("raw-image-data", raw_data_changed, PAGE_READWRITE, raw_data_old,
                      raw_data_error, raw_data_query, &raw_data_mbi);
    report_protection("facade-image-data", facade_data_changed, PAGE_READWRITE, facade_data_old,
                      facade_data_error, facade_data_query, &facade_data_mbi);
    check(raw_data_changed && facade_data_changed && raw_data_query && facade_data_query &&
          raw_data_old == PAGE_READONLY && facade_data_old == raw_data_old &&
          raw_data_mbi.Protect == facade_data_mbi.Protect,
          "ordinary data behavior exactly matches raw KERNEL32 image mapping");
    if (!VirtualProtect((LPVOID)data, sizeof(*data), data_original, &old) ||
        !VirtualProtect((LPVOID)control_data, sizeof(*control_data), control_data_original, &old)) {
        printf("FAIL restore ordinary data controls error=%lu\n", GetLastError());
        return 2;
    }

    SYSTEM_INFO range_info;
    GetSystemInfo(&range_info);
    uintptr_t range_start = (uintptr_t)code & ~((uintptr_t)range_info.dwPageSize - 1);
    uintptr_t control_range_start = (uintptr_t)control_code & ~((uintptr_t)range_info.dwPageSize - 1);
    uintptr_t range_end = ((uintptr_t)data + sizeof(*data) + range_info.dwPageSize - 1) &
                          ~((uintptr_t)range_info.dwPageSize - 1);
    uintptr_t control_range_end = ((uintptr_t)control_data + sizeof(*control_data) + range_info.dwPageSize - 1) &
                                  ~((uintptr_t)range_info.dwPageSize - 1);
    DWORD saved_pages[32], control_saved_pages[32];
    unsigned saved_count = 0, control_saved_count = 0;
    for (uintptr_t page = range_start; page < range_end; page += range_info.dwPageSize) {
        if (saved_count == sizeof(saved_pages) / sizeof(saved_pages[0]) ||
            !query_protect((LPCVOID)page, &saved_pages[saved_count], NULL)) {
            printf("FAIL inspect facade cross-section protection range\n");
            return 2;
        }
        ++saved_count;
    }
    for (uintptr_t page = control_range_start; page < control_range_end; page += range_info.dwPageSize) {
        if (control_saved_count == sizeof(control_saved_pages) / sizeof(control_saved_pages[0]) ||
            !query_protect((LPCVOID)page, &control_saved_pages[control_saved_count], NULL)) {
            printf("FAIL inspect raw cross-section protection range\n");
            return 2;
        }
        ++control_saved_count;
    }
    SIZE_T cross_size = (SIZE_T)((uintptr_t)data + sizeof(*data) - (uintptr_t)code);
    SIZE_T control_cross_size = (SIZE_T)((uintptr_t)control_data + sizeof(*control_data) -
                                         (uintptr_t)control_code);
    DWORD raw_cross_old = 0, facade_cross_old = 0;
    BOOL raw_cross_changed = VirtualProtect((LPVOID)control_code, control_cross_size,
                                            PAGE_READWRITE, &raw_cross_old);
    DWORD raw_cross_error = GetLastError();
    MEMORY_BASIC_INFORMATION raw_cross_mbi, raw_cross_data_mbi;
    int raw_cross_query = query_protect((LPCVOID)control_code, NULL, &raw_cross_mbi);
    int raw_cross_data_query = query_protect((LPCVOID)control_data, NULL, &raw_cross_data_mbi);
    BOOL facade_cross_changed = wrapped_protect((LPVOID)code, cross_size,
                                                PAGE_READWRITE, &facade_cross_old);
    DWORD facade_cross_error = GetLastError();
    MEMORY_BASIC_INFORMATION facade_cross_mbi, facade_cross_data_mbi;
    int facade_cross_query = query_protect((LPCVOID)code, NULL, &facade_cross_mbi);
    int facade_cross_data_query = query_protect((LPCVOID)data, NULL, &facade_cross_data_mbi);
    report_protection("raw-cross-section", raw_cross_changed, PAGE_READWRITE, raw_cross_old,
                      raw_cross_error, raw_cross_query, &raw_cross_mbi);
    report_protection("facade-cross-section", facade_cross_changed, PAGE_READWRITE, facade_cross_old,
                      facade_cross_error, facade_cross_query, &facade_cross_mbi);
    report_protection("raw-cross-data-page", raw_cross_changed, PAGE_READWRITE, raw_cross_old,
                      raw_cross_error, raw_cross_data_query, &raw_cross_data_mbi);
    report_protection("facade-cross-data-page", facade_cross_changed, PAGE_READWRITE, facade_cross_old,
                      facade_cross_error, facade_cross_data_query, &facade_cross_data_mbi);
    check(raw_cross_changed && facade_cross_changed && raw_cross_query && facade_cross_query &&
          raw_cross_data_query && facade_cross_data_query && raw_cross_old == facade_cross_old &&
          raw_cross_mbi.Protect == facade_cross_mbi.Protect &&
          raw_cross_data_mbi.Protect == facade_cross_data_mbi.Protect,
          "cross-section code and data protections exactly match raw KERNEL32 control");
    for (unsigned i = 0; i < saved_count; ++i) {
        DWORD ignored = 0;
        if (!VirtualProtect((LPVOID)(range_start + (uintptr_t)i * range_info.dwPageSize),
                            range_info.dwPageSize, saved_pages[i], &ignored)) {
            printf("FAIL restore facade cross-section page %u error=%lu\n", i, GetLastError());
            return 2;
        }
    }
    for (unsigned i = 0; i < control_saved_count; ++i) {
        DWORD ignored = 0;
        if (!VirtualProtect((LPVOID)(control_range_start + (uintptr_t)i * range_info.dwPageSize),
                            range_info.dwPageSize, control_saved_pages[i], &ignored)) {
            printf("FAIL restore raw cross-section page %u error=%lu\n", i, GetLastError());
            return 2;
        }
    }

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    LPVOID private_page = VirtualAlloc(NULL, si.dwPageSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(private_page != NULL, "private page allocation");
    if (!private_page) return 2;
    *(volatile BYTE *)private_page = 0x5a;
    DWORD modes[] = { PAGE_READONLY, PAGE_NOACCESS, PAGE_READONLY | PAGE_GUARD };
    const char *labels[] = { "private read-only memory remains non-executable",
                             "no-access protection is preserved",
                             "guard protection is preserved" };
    for (unsigned i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        if (!VirtualProtect(private_page, si.dwPageSize, modes[i], &old)) {
            printf("FAIL set private protection 0x%lx error=%lu\n", modes[i], GetLastError());
            ++failures;
            ++cases;
            continue;
        }
        DWORD private_real = 0;
        MEMORY_BASIC_INFORMATION private_facade;
        int private_ok = query_protect(private_page, &private_real, NULL);
        SIZE_T private_got = wrapped_query(private_page, &private_facade, sizeof(private_facade));
        check(private_ok && private_real == modes[i] && private_got == sizeof(private_facade) &&
              private_facade.Protect == modes[i], labels[i]);
    }
    VirtualProtect(private_page, si.dwPageSize, PAGE_READWRITE, &old);
    VirtualFree(private_page, 0, MEM_RELEASE);
    printf("SUMMARY same-directory: %d/%d checks failed\n", failures, cases);
    return failures ? 1 : 0;
}

typedef struct other_thread_case {
    facade_virtual_query_fn query;
    LPVOID page;
    SIZE_T size;
    int fresh;
} other_thread_case;

static DWORD WINAPI other_thread_queries(LPVOID context) {
    other_thread_case *c = (other_thread_case *)context;
    MEMORY_BASIC_INFORMATION first, second;
    DWORD old;
    c->fresh = c->query(c->page, &first, sizeof(first)) == sizeof(first) &&
               VirtualProtect(c->page, c->size, PAGE_READONLY, &old) &&
               c->query(c->page, &second, sizeof(second)) == sizeof(second) &&
               first.Protect == PAGE_READWRITE && second.Protect == PAGE_READONLY;
    VirtualProtect(c->page, c->size, PAGE_READWRITE, &old);
    return 0;
}

/* Checks the query cache. lifetime_ms must match MP2VRCMP_VQ_CACHE_MS in the
 * environment; 0 checks that a disabled cache always answers afresh. */
static int run_cache(unsigned lifetime_ms) {
    facade_virtual_query_fn wrapped_query;
    facade_virtual_protect_fn wrapped_protect;
    if (!load_facade(&wrapped_query, &wrapped_protect)) return 2;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    SIZE_T size = si.dwPageSize;
    BYTE *page = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    BYTE *other = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(page && other, "private page allocation");
    if (!page || !other) return 2;
    page[0] = other[0] = 0x5a;
    MEMORY_BASIC_INFORMATION a, b, raw;
    DWORD old;
    Sleep(100); /* the facade measures its clock over its first 20 ms */

    SIZE_T got = wrapped_query(page + 17, &a, sizeof(a));
    check(got == sizeof(a) && VirtualQuery(page + 17, &raw, sizeof(raw)) == sizeof(raw) &&
          memcmp(&a, &raw, sizeof(raw)) == 0, "first answer equals the direct query");
    got = wrapped_query(page + 900, &b, sizeof(b));
    check(got == sizeof(b) && memcmp(&a, &b, sizeof(a)) == 0, "repeated answer is identical");

    int changed = VirtualProtect(page, size, PAGE_READONLY, &old);
    got = wrapped_query(page, &b, sizeof(b));
    if (lifetime_ms)
        check(changed && got == sizeof(b) && b.Protect == PAGE_READWRITE,
              "a repeated query inside the lifetime is answered from the cache");
    else
        check(changed && got == sizeof(b) && b.Protect == PAGE_READONLY,
              "a disabled cache answers afresh");
    Sleep(lifetime_ms * 2 + 30);
    got = wrapped_query(page, &b, sizeof(b));
    check(got == sizeof(b) && b.Protect == PAGE_READONLY, "an answer older than the lifetime is not reused");

    got = wrapped_query(page, &a, sizeof(a));
    changed = wrapped_protect(page, size, PAGE_READWRITE, &old);
    got = wrapped_query(page, &b, sizeof(b));
    check(changed && got == sizeof(b) && b.Protect == PAGE_READWRITE,
          "VirtualProtect through the facade drops cached answers");

    got = wrapped_query(page, &a, sizeof(a));
    changed = VirtualFree(page, size, MEM_DECOMMIT);
    got = wrapped_query(page, &b, sizeof(b));
    check(a.State == MEM_COMMIT && changed && got == sizeof(b) && b.State == MEM_RESERVE,
          "a decommitted page is never reported committed");
    got = wrapped_query(page, &a, sizeof(a));
    check(got == sizeof(a) && a.State == MEM_RESERVE, "reserved answers stay correct when repeated");

    other_thread_case c = { wrapped_query, other, size, 0 };
    HANDLE thread = CreateThread(NULL, 0, other_thread_queries, &c, 0, NULL);
    if (thread) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
    check(thread && c.fresh, "queries from other threads are always answered afresh");

    int local = 0;
    got = wrapped_query(&local, &a, sizeof(a));
    check(got == sizeof(a) && VirtualQuery(&local, &raw, sizeof(raw)) == sizeof(raw) &&
          memcmp(&a, &raw, sizeof(raw)) == 0 && wrapped_query(&local, &b, sizeof(b)) == sizeof(b) &&
          memcmp(&b, &raw, sizeof(raw)) == 0, "stack queries equal the direct query");
    check(wrapped_query(page, &a, 4) == VirtualQuery(page, &raw, 4), "short buffers are passed through");

    VirtualFree(page, 0, MEM_RELEASE);
    VirtualFree(other, 0, MEM_RELEASE);
    printf("SUMMARY cache lifetime=%u: %d/%d checks failed\n", lifetime_ms, failures, cases);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "same") == 0) return run_same_directory();
    if (argc == 3 && strcmp(argv[1], "outside") == 0) return run_outside(argv[2]);
    if (argc == 3 && strcmp(argv[1], "cache") == 0) return run_cache((unsigned)strtoul(argv[2], NULL, 10));
    fprintf(stderr, "usage: maxpayne2.exe same | outside <absolute-engine-dll-path> | cache <lifetime-ms>\n");
    return 2;
}
