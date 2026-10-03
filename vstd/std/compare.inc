// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

export namespace std {

  // [cmp.categories], comparison category types
  using std::partial_ordering;
  using std::strong_ordering;
  using std::weak_ordering;

  // named comparison functions
  using std::is_eq;
  using std::is_gt;
  using std::is_gteq;
  using std::is_lt;
  using std::is_lteq;
  using std::is_neq;

  // [cmp.common], common comparison category type
  using std::common_comparison_category;
  using std::common_comparison_category_t;

  // [cmp.concept], concept three_way_comparable
  using std::three_way_comparable;
  using std::three_way_comparable_with;

  // [cmp.result], result of three-way comparison
  using std::compare_three_way_result;

  using std::compare_three_way_result_t;

  // [comparisons.three.way], class compare_three_way
  using std::compare_three_way;

  // [cmp.alg], comparison algorithms
  //
  // PORTABILITY LAYER (vstd/vstd_compat.inc): this block used to re-export libc++'s INTERNAL spelling
  // inside `inline namespace __cpo` - six `using std::__cpo::...` lines, the one place in the whole
  // partition set that named something below the standard's own surface. `__cpo` is an inline
  // namespace in libc++, so `std::partial_order` and `std::__cpo::partial_order` are the same entity:
  // naming the standard spelling here exports exactly what it exported before on libc++, and it is the
  // spelling every other standard library provides.
  using std::compare_partial_order_fallback;
  using std::compare_strong_order_fallback;
  using std::compare_weak_order_fallback;
  using std::partial_order;
  using std::strong_order;
  using std::weak_order;

} // namespace std
