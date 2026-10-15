/* SPDX-License-Identifier: MIT */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "permission_policy.h"

#define MAX_ENGINE_SECTIONS 96
#define MAX_OPTIONAL_HEADER 4096
#define MAX_NT_OFFSET (1024u * 1024u)

typedef struct section_span {
    uintptr_t begin;
    uintptr_t end;
    int executable;
} section_span;

typedef struct image_sections {
    uintptr_t base;
    uintptr_t image_end;
    unsigned count;
    section_span spans[MAX_ENGINE_SECTIONS];
} image_sections;

static int safe_read(HANDLE process, uintptr_t address, void *out, SIZE_T size) {
    SIZE_T got = 0;
    return size && ReadProcessMemory(process, (LPCVOID)address, out, size, &got) && got == size;
}

static const char *path_basename(const char *path) {
    const char *name = path + strlen(path);
    while (name > path && name[-1] != '\\' && name[-1] != '/') --name;
    return name;
}

static int same_directory_as_game(const char *module_path) {
    char game_path[MAX_PATH];
    DWORD n = GetModuleFileNameA(GetModuleHandleA(NULL), game_path, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    const char *game_name = path_basename(game_path);
    const char *module_name = path_basename(module_path);
    size_t game_dir_len = (size_t)(game_name - game_path);
    size_t module_dir_len = (size_t)(module_name - module_path);
    return game_dir_len == module_dir_len &&
           _strnicmp(game_path, module_path, game_dir_len) == 0;
}

static int allowed_engine_module(uintptr_t base) {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA((HMODULE)base, path, MAX_PATH);
    if (!n || n >= MAX_PATH || !same_directory_as_game(path)) return 0;
    const char *name = path_basename(path);
    if (_stricmp(name, "maxpayne2.exe") == 0 ||
        _stricmp(name, "X_ModesMFC.dll") == 0 ||
        _stricmp(name, "X_Inputmfc.dll") == 0 ||
        _stricmp(name, "X_BasicModesMFC.dll") == 0 ||
        _stricmp(name, "e2mfc.dll") == 0 ||
        _stricmp(name, "e2driver.dll") == 0) return 1;
    size_t len = strlen(name);
    return len > 8 && _strnicmp(name, "X_", 2) == 0 &&
           _stricmp(name + len - 7, "MFC.dll") == 0;
}

static int parse_engine_sections(uintptr_t base, image_sections *out) {
    HANDLE process = GetCurrentProcess();
    IMAGE_DOS_HEADER dos;
    DWORD signature;
    IMAGE_FILE_HEADER file;
    BYTE optional[MAX_OPTIONAL_HEADER];
    IMAGE_SECTION_HEADER headers[MAX_ENGINE_SECTIONS];
    if (!safe_read(process, base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew <= 0 || (DWORD)dos.e_lfanew > MAX_NT_OFFSET) return 0;
    uintptr_t nt = base + (uintptr_t)(DWORD)dos.e_lfanew;
    if (nt < base || nt > UINTPTR_MAX - (sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + MAX_OPTIONAL_HEADER +
                                          MAX_ENGINE_SECTIONS * sizeof(IMAGE_SECTION_HEADER)) || !safe_read(process, nt, &signature, sizeof(signature)) ||
        signature != IMAGE_NT_SIGNATURE ||
        !safe_read(process, nt + sizeof(DWORD), &file, sizeof(file)) ||
        file.Machine != IMAGE_FILE_MACHINE_I386 || !file.NumberOfSections ||
        file.NumberOfSections > MAX_ENGINE_SECTIONS ||
        file.SizeOfOptionalHeader < 96 || file.SizeOfOptionalHeader > MAX_OPTIONAL_HEADER ||
        !safe_read(process, nt + sizeof(DWORD) + sizeof(file), optional, file.SizeOfOptionalHeader)) return 0;
    if (*(WORD *)optional != IMAGE_NT_OPTIONAL_HDR32_MAGIC) return 0;
    DWORD image_size = *(DWORD *)(optional + 56);
    DWORD section_alignment = *(DWORD *)(optional + 32);
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    DWORD page_size = system_info.dwPageSize;
    if (!image_size || !section_alignment || !page_size ||
        (section_alignment & (section_alignment - 1)) != 0 ||
        section_alignment < page_size || section_alignment % page_size != 0 ||
        section_alignment > image_size || base > UINTPTR_MAX - image_size) return 0;
    uintptr_t sections_at = nt + sizeof(DWORD) + sizeof(file) + file.SizeOfOptionalHeader;
    if (sections_at < nt || !safe_read(process, sections_at, headers,
                                       (SIZE_T)file.NumberOfSections * sizeof(headers[0]))) return 0;
    for (unsigned i = 0; i < file.NumberOfSections; ++i) {
        DWORD va = headers[i].VirtualAddress;
        DWORD length = headers[i].Misc.VirtualSize;
        if (va >= image_size || va % section_alignment != 0 || length > image_size - va) return 0;
        if (!length) continue;
        for (unsigned j = i + 1; j < file.NumberOfSections; ++j) {
            if (headers[j].VirtualAddress == va && headers[j].Misc.VirtualSize) return 0;
        }
    }

    out->base = base;
    out->image_end = base + image_size;
    out->count = 0;
    for (unsigned i = 0; i < file.NumberOfSections; ++i) {
        DWORD va = headers[i].VirtualAddress;
        DWORD length = headers[i].Misc.VirtualSize;
        if (!length) continue;
        DWORD section_end = va + length;
        DWORD next_section = image_size;
        for (unsigned j = 0; j < file.NumberOfSections; ++j) {
            DWORD candidate = headers[j].VirtualAddress;
            if (candidate > va && candidate < next_section) next_section = candidate;
        }
        if (section_end > next_section) return 0;
        DWORD mapped_end = section_end;
        int executable = (headers[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (executable) {
            DWORD alignment_mask = section_alignment - 1;
            if (section_end > UINT32_MAX - alignment_mask) return 0;
            mapped_end = (section_end + alignment_mask) & ~alignment_mask;
            if (mapped_end > image_size) mapped_end = image_size;
            if (mapped_end > next_section) mapped_end = next_section;
        }
        if (mapped_end <= va) continue;
        uintptr_t begin = base + va;
        uintptr_t end = base + mapped_end;
        if (begin < base || end < begin || end > out->image_end) continue;
        section_span *span = &out->spans[out->count++];
        span->begin = begin;
        span->end = end;
        span->executable = executable;
    }
    return out->count != 0;
}

static int declared_executable(uintptr_t allocation_base, uintptr_t address, SIZE_T size) {
    if (!allocation_base || !size || address < allocation_base || address > UINTPTR_MAX - size) return 0;
    image_sections image;
    if (!parse_engine_sections(allocation_base, &image) || address + size > image.image_end) return 0;
    for (unsigned i = 0; i < image.count; ++i) {
        section_span *s = &image.spans[i];
        if (s->executable && address >= s->begin && address + size <= s->end) return 1;
    }
    return 0;
}

static int committed_engine_image_range(LPVOID address, SIZE_T size, uintptr_t *allocation_base) {
    if (!address || !size || (uintptr_t)address > UINTPTR_MAX - size) return 0;
    uintptr_t cursor = (uintptr_t)address;
    uintptr_t end = cursor + size;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((LPCVOID)cursor, &mbi, sizeof(mbi)) != sizeof(mbi) ||
        mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE || !mbi.AllocationBase ||
        !allowed_engine_module((uintptr_t)mbi.AllocationBase)) return 0;
    uintptr_t base = (uintptr_t)mbi.AllocationBase;
    while (cursor < end) {
        SIZE_T got = VirtualQuery((LPCVOID)cursor, &mbi, sizeof(mbi));
        if (got != sizeof(mbi) || mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE ||
            (uintptr_t)mbi.AllocationBase != base || mbi.RegionSize == 0 ||
            (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) ||
            (mbi.Protect & MP2_PAGE_BASE_MASK) == PAGE_NOACCESS) return 0;
        uintptr_t region = (uintptr_t)mbi.BaseAddress;
        if (region > cursor || region > UINTPTR_MAX - mbi.RegionSize) return 0;
        uintptr_t next = region + mbi.RegionSize;
        if (next <= cursor) return 0;
        cursor = next < end ? next : end;
    }
    *allocation_base = base;
    return declared_executable(base, (uintptr_t)address, size);
}

/* VirtualQuery result cache.
 *
 * The mod guards its per-frame reads of engine objects with VirtualQuery and ships a
 * "stereo hot-path cache" for those queries. That cache cannot install once the mod's
 * KERNEL32 imports resolve to this facade, and under Proton on ARM64 every query is a
 * WOW64 system call, so the facade keeps the equivalent cache itself.
 *
 * Results are kept per queried page, for a few milliseconds, for the thread that loaded
 * the facade only (no locking). A hit on a readable page is confirmed by reading the
 * queried byte, so a page unmapped since it was cached can never be reported readable.
 * Every Virtual* call that goes through the facade drops the whole cache.
 * MP2VRCMP_VQ_CACHE_MS sets the lifetime (0 disables, default 8, at most 1000). */
#define VQ_SLOT_BITS 11
#define VQ_SLOTS (1u << VQ_SLOT_BITS)
#define VQ_PAGE_MASK ((uintptr_t)0xfff)
#define VQ_DEFAULT_MS 8u
#define VQ_MAX_MS 1000u
#define VQ_CALIBRATION_QPC_MS 20

typedef struct vq_entry {
    uintptr_t page;
    uint32_t generation;
    uint32_t probe;
    uint64_t stamp;
    MEMORY_BASIC_INFORMATION info;
} vq_entry;

static vq_entry vq_cache[VQ_SLOTS];
static uint32_t vq_generation = 1;
static DWORD vq_thread;
static uint32_t vq_lifetime_ms;
static uint64_t vq_lifetime_ticks;
static uint64_t vq_ticks_per_ms;
static uint64_t vq_calibration_tsc;
static LARGE_INTEGER vq_calibration_qpc;
static LARGE_INTEGER vq_qpc_frequency;
static DWORD vq_page_size;

static struct {
    uint32_t enabled;
    uint32_t hits, misses, other_thread, probe_faults, flushes;
    uint64_t next_report;
} vq_stats;

/* int mp2_probe_read(const void *p): 1 when *p can be read, 0 when the read faults. */
__asm__(".text\n"
        ".globl _mp2_probe_read\n"
        "_mp2_probe_read:\n"
        "    movl 4(%esp), %eax\n"
        "    movzbl (%eax), %eax\n"
        "    movl $1, %eax\n"
        "    ret\n"
        ".globl _mp2_probe_fault\n"
        "_mp2_probe_fault:\n"
        "    xorl %eax, %eax\n"
        "    ret\n");
extern int __cdecl mp2_probe_read(const void *address);
extern int __cdecl mp2_probe_fault(void);

static LONG CALLBACK vq_probe_handler(EXCEPTION_POINTERS *exception) {
    uintptr_t ip = (uintptr_t)exception->ContextRecord->Eip;
    DWORD code = exception->ExceptionRecord->ExceptionCode;
    if (ip < (uintptr_t)mp2_probe_read || ip >= (uintptr_t)mp2_probe_fault ||
        (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_GUARD_PAGE &&
         code != EXCEPTION_IN_PAGE_ERROR)) return EXCEPTION_CONTINUE_SEARCH;
    exception->ContextRecord->Eip = (DWORD)(uintptr_t)mp2_probe_fault;
    return EXCEPTION_CONTINUE_EXECUTION;
}

static inline uint64_t vq_now(void) { return __builtin_ia32_rdtsc(); }

static void vq_flush(void) {
    if (++vq_generation == 0) {
        memset(vq_cache, 0, sizeof(vq_cache));
        vq_generation = 1;
    }
    ++vq_stats.flushes;
}

static uint32_t vq_env_number(const char *name, uint32_t fallback, uint32_t limit) {
    char text[16];
    DWORD n = GetEnvironmentVariableA(name, text, sizeof(text));
    if (!n || n >= sizeof(text)) return fallback;
    uint32_t value = 0;
    for (DWORD i = 0; i < n; ++i) {
        if (text[i] < '0' || text[i] > '9') return fallback;
        value = value * 10u + (uint32_t)(text[i] - '0');
        if (value > limit) return fallback;
    }
    return value;
}

static void vq_init(void) {
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    vq_page_size = system_info.dwPageSize;
    vq_thread = GetCurrentThreadId();
    vq_lifetime_ms = vq_env_number("MP2VRCMP_VQ_CACHE_MS", VQ_DEFAULT_MS, VQ_MAX_MS);
    vq_stats.enabled = vq_env_number("MP2VRCMP_STATS", 0, 1);
    if (vq_page_size != VQ_PAGE_MASK + 1 || !QueryPerformanceFrequency(&vq_qpc_frequency) ||
        vq_qpc_frequency.QuadPart <= 0 || !QueryPerformanceCounter(&vq_calibration_qpc) ||
        !AddVectoredExceptionHandler(1, vq_probe_handler)) vq_lifetime_ms = 0;
    vq_calibration_tsc = vq_now();
}

/* The time-stamp counter has no documented rate here, so measure it against the
 * performance counter once enough time has passed. Until then nothing is cached. */
static int vq_calibrated(uint64_t now) {
    if (vq_lifetime_ticks) return 1;
    LARGE_INTEGER qpc;
    if (!QueryPerformanceCounter(&qpc)) return 0;
    int64_t elapsed = qpc.QuadPart - vq_calibration_qpc.QuadPart;
    if (elapsed * 1000 < vq_qpc_frequency.QuadPart * VQ_CALIBRATION_QPC_MS || now <= vq_calibration_tsc) return 0;
    vq_ticks_per_ms = (now - vq_calibration_tsc) * (uint64_t)vq_qpc_frequency.QuadPart /
                      ((uint64_t)elapsed * 1000u);
    if (!vq_ticks_per_ms) {
        vq_lifetime_ms = 0;
        return 0;
    }
    vq_lifetime_ticks = vq_ticks_per_ms * vq_lifetime_ms;
    vq_stats.next_report = now + vq_ticks_per_ms * 5000u;
    return 1;
}

static void vq_report(uint64_t now) {
    char line[160];
    DWORD written;
    vq_stats.next_report = now + vq_ticks_per_ms * 5000u;
    HANDLE file = CreateFileA("mp2vrcmp-stats.log", FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    int n = wsprintfA(line, "vq lifetime_ms=%u hits=%u misses=%u other_thread=%u probe_faults=%u flushes=%u\r\n",
                      vq_lifetime_ms, vq_stats.hits, vq_stats.misses, vq_stats.other_thread,
                      vq_stats.probe_faults, vq_stats.flushes);
    if (n > 0) WriteFile(file, line, (DWORD)n, &written, NULL);
    CloseHandle(file);
    vq_stats.hits = vq_stats.misses = vq_stats.other_thread = vq_stats.probe_faults = vq_stats.flushes = 0;
}

static inline vq_entry *vq_slot(uintptr_t page) {
    return &vq_cache[(uint32_t)((page >> 12) * 2654435761u) >> (32 - VQ_SLOT_BITS)];
}

#define VQ_STACK_MARGIN ((uintptr_t)1 << 20)

/* The guard page below a stack's committed part moves as the stack grows, and reading
 * it consumes the guard. Never cache or probe this thread's stack. */
static int vq_on_stack(uintptr_t address) {
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    uintptr_t here = (uintptr_t)&tib;
    uintptr_t low = here > VQ_STACK_MARGIN ? here - VQ_STACK_MARGIN : 0;
    return address >= low && address < (uintptr_t)tib->StackBase;
}

static int vq_lookup(LPCVOID address, PMEMORY_BASIC_INFORMATION info) {
    uintptr_t page = (uintptr_t)address & ~VQ_PAGE_MASK;
    vq_entry *entry = vq_slot(page);
    uint64_t now = vq_now();
    if (vq_stats.enabled && vq_ticks_per_ms && now >= vq_stats.next_report) vq_report(now);
    if (entry->page != page || entry->generation != vq_generation ||
        now - entry->stamp >= vq_lifetime_ticks) return 0;
    if (entry->probe && !mp2_probe_read(address)) {
        entry->generation = 0;
        ++vq_stats.probe_faults;
        return 0;
    }
    *info = entry->info;
    return 1;
}

static void vq_store(LPCVOID address, const MEMORY_BASIC_INFORMATION *info) {
    uint64_t now = vq_now();
    if (!vq_calibrated(now) || vq_on_stack((uintptr_t)address)) return;
    uintptr_t page = (uintptr_t)address & ~VQ_PAGE_MASK;
    DWORD base_protect = info->Protect & MP2_PAGE_BASE_MASK;
    vq_entry *entry = vq_slot(page);
    entry->page = page;
    entry->generation = vq_generation;
    entry->stamp = now;
    entry->info = *info;
    entry->probe = info->State == MEM_COMMIT && !(info->Protect & PAGE_GUARD) &&
                   base_protect != 0 && base_protect != PAGE_NOACCESS && base_protect != PAGE_EXECUTE;
}

static SIZE_T query_uncached(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length) {
    SIZE_T result = VirtualQuery(address, info, length);
    DWORD saved_error = GetLastError();
    if (result == sizeof(MEMORY_BASIC_INFORMATION) && info &&
        info->State == MEM_COMMIT && info->Type == MEM_IMAGE &&
        !(info->Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
        uintptr_t base = (uintptr_t)info->AllocationBase;
        SIZE_T page_size = vq_page_size;
        uintptr_t queried = (uintptr_t)address;
        uintptr_t page = page_size ? queried - queried % page_size : 0;
        if (page_size && allowed_engine_module(base) &&
            declared_executable(base, page, page_size))
            info->Protect = mp2_declared_protection(info->Protect, 1);
    }
    SetLastError(saved_error);
    return result;
}

SIZE_T WINAPI mp2_VirtualQuery(LPCVOID address, PMEMORY_BASIC_INFORMATION info, SIZE_T length) {
    int cacheable = vq_lifetime_ms && info && length == sizeof(MEMORY_BASIC_INFORMATION);
    if (cacheable && GetCurrentThreadId() != vq_thread) {
        ++vq_stats.other_thread;
        cacheable = 0;
    }
    if (cacheable && vq_lifetime_ticks && vq_lookup(address, info)) {
        ++vq_stats.hits;
        return sizeof(MEMORY_BASIC_INFORMATION);
    }
    SIZE_T result = query_uncached(address, info, length);
    if (cacheable) {
        ++vq_stats.misses;
        if (result == sizeof(MEMORY_BASIC_INFORMATION)) {
            DWORD saved_error = GetLastError();
            vq_store(address, info);
            SetLastError(saved_error);
        }
    }
    return result;
}

BOOL WINAPI mp2_VirtualProtect(LPVOID address, SIZE_T size, DWORD new_protect, PDWORD old_protect) {
    DWORD saved_error = GetLastError();
    DWORD requested = new_protect;
    DWORD base_protect = new_protect & MP2_PAGE_BASE_MASK;
    if (!(new_protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
        (base_protect == PAGE_READONLY || base_protect == PAGE_READWRITE || base_protect == PAGE_WRITECOPY)) {
        uintptr_t base = 0;
        if (committed_engine_image_range(address, size, &base))
            requested = mp2_declared_protection(new_protect, 1);
    }
    vq_flush();
    SetLastError(saved_error);
    return VirtualProtect(address, size, requested, old_protect);
}

LPVOID WINAPI mp2_VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protect) {
    vq_flush();
    return VirtualAlloc(address, size, type, protect);
}

BOOL WINAPI mp2_VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
    vq_flush();
    return VirtualFree(address, size, type);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)instance;
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) vq_init();
    return TRUE;
}
