/* SPDX-License-Identifier: MIT */
#include <windows.h>
#include <stdint.h>

__declspec(dllexport) volatile uint32_t facade_test_data = 0x1234abcd;

__declspec(dllexport) uint32_t WINAPI facade_test_code(uint32_t input) {
    return input ^ 0x5a39c7e1u;
}
