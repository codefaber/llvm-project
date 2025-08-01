//===-- Shared asinf16 function ---------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIBC_SHARED_MATH_ASINF16_H
#define LLVM_LIBC_SHARED_MATH_ASINF16_H

#include "shared/libc_common.h"

#ifdef LIBC_TYPES_HAS_FLOAT16

#include "src/__support/math/asinf16.h"

namespace LIBC_NAMESPACE_DECL {
namespace shared {

using math::asinf16;

} // namespace shared
} // namespace LIBC_NAMESPACE_DECL

#endif // LIBC_TYPES_HAS_FLOAT16

#endif // LLVM_LIBC_SHARED_MATH_ASINF_H
