/* SPDX-License-Identifier: MIT */
#ifndef MP2VRCMP_PERMISSION_POLICY_H
#define MP2VRCMP_PERMISSION_POLICY_H

#include <stdint.h>

#define MP2_PAGE_NOACCESS       0x01u
#define MP2_PAGE_READONLY       0x02u
#define MP2_PAGE_READWRITE      0x04u
#define MP2_PAGE_WRITECOPY      0x08u
#define MP2_PAGE_EXECUTE_READ   0x20u
#define MP2_PAGE_EXECUTE_RW     0x40u
#define MP2_PAGE_EXECUTE_COPY   0x80u
#define MP2_PAGE_GUARD          0x100u
#define MP2_PAGE_BASE_MASK      0xffu

/* Apply only when caller separately proved that the complete range lies in a
 * committed executable PE section of an allowed engine module. */
static inline uint32_t mp2_declared_protection(uint32_t protect, int executable_declared) {
    if (!executable_declared || (protect & MP2_PAGE_GUARD))
        return protect;
    uint32_t modifiers = protect & ~MP2_PAGE_BASE_MASK;
    switch (protect & MP2_PAGE_BASE_MASK) {
        case MP2_PAGE_READONLY:  return modifiers | MP2_PAGE_EXECUTE_READ;
        case MP2_PAGE_READWRITE: return modifiers | MP2_PAGE_EXECUTE_RW;
        case MP2_PAGE_WRITECOPY: return modifiers | MP2_PAGE_EXECUTE_COPY;
        default:                 return protect;
    }
}
#endif
