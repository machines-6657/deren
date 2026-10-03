// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

export namespace std {
#if VSTD_STD_VER >= 23
  // [print.fun], print functions
  using std::print;
  using std::println;

  using std::vprint_nonunicode;
#  if VSTD_HAS_UNICODE
  using std::vprint_unicode;
#  endif // VSTD_HAS_UNICODE
#endif   // VSTD_STD_VER >= 23
} // namespace std
