/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include "../src/permission_policy.h"

int main(void) {
    assert(mp2_declared_protection(MP2_PAGE_READONLY, 1) == MP2_PAGE_EXECUTE_READ);
    assert(mp2_declared_protection(MP2_PAGE_READWRITE, 1) == MP2_PAGE_EXECUTE_RW);
    assert(mp2_declared_protection(MP2_PAGE_WRITECOPY, 1) == MP2_PAGE_EXECUTE_COPY);
    assert(mp2_declared_protection(MP2_PAGE_READONLY | 0x200u, 1) == (MP2_PAGE_EXECUTE_READ | 0x200u));
    assert(mp2_declared_protection(MP2_PAGE_READONLY, 0) == MP2_PAGE_READONLY);
    assert(mp2_declared_protection(MP2_PAGE_READWRITE | MP2_PAGE_GUARD, 1) == (MP2_PAGE_READWRITE | MP2_PAGE_GUARD));
    assert(mp2_declared_protection(MP2_PAGE_NOACCESS, 1) == MP2_PAGE_NOACCESS);
    assert(mp2_declared_protection(MP2_PAGE_EXECUTE_READ, 1) == MP2_PAGE_EXECUTE_READ);
    return 0;
}
