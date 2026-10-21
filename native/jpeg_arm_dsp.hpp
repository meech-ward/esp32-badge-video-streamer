// SPDX-License-Identifier: MIT
#pragma once

// JPEGDEC selects its packed RGB565 conversion using its historical M4/M7
// macro. Only enable that existing path on a compiler-confirmed DSP-capable M33.
#if !defined(__ARM_ARCH_8M_MAIN__) || !defined(__ARM_FEATURE_DSP)
#error "JPEGDEC DSP needs Armv8-M Mainline with the DSP extension"
#endif

#include <arm_acle.h>
#include <cstdint>

#define ARM_MATH_CM4 1
#define __SSUB16(a, b) static_cast<std::uint32_t>(__ssub16(static_cast<std::int32_t>(a), static_cast<std::int32_t>(b)))
#define __SMLAD(a, b, c) static_cast<std::uint32_t>(__smlad(static_cast<std::int32_t>(a), static_cast<std::int32_t>(b), static_cast<std::int32_t>(c)))
#define __USAT16(a, n) static_cast<std::uint32_t>(__usat16(static_cast<std::int32_t>(a), n))
