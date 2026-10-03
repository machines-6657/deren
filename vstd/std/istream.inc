// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

export namespace std {
#if VSTD_HAS_LOCALIZATION
  using std::basic_istream;

  using std::istream;
#  if VSTD_HAS_WIDE_CHARACTERS
  using std::wistream;
#  endif

  using std::basic_iostream;

  using std::iostream;
#  if VSTD_HAS_WIDE_CHARACTERS
  using std::wiostream;
#  endif

  using std::ws;

  using std::operator>>;
#endif // VSTD_HAS_LOCALIZATION
} // namespace std
