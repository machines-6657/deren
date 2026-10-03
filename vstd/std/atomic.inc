// -*- C++ -*-
//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

export namespace std {

  // [atomics.order], order and consistency
  using std::memory_order VSTD_USING_IF_EXISTS;
  using std::memory_order_acq_rel VSTD_USING_IF_EXISTS;
  using std::memory_order_acquire VSTD_USING_IF_EXISTS;
  using std::memory_order_consume VSTD_USING_IF_EXISTS;
  using std::memory_order_relaxed VSTD_USING_IF_EXISTS;
  using std::memory_order_release VSTD_USING_IF_EXISTS;
  using std::memory_order_seq_cst VSTD_USING_IF_EXISTS;

  using std::kill_dependency VSTD_USING_IF_EXISTS;

  // [atomics.ref.generic], class template atomic_ref
  // [atomics.ref.pointer], partial specialization for pointers
  using std::atomic_ref VSTD_USING_IF_EXISTS;

  // [atomics.types.generic], class template atomic
  using std::atomic VSTD_USING_IF_EXISTS;

  // [atomics.nonmembers], non-member functions
  using std::atomic_compare_exchange_strong VSTD_USING_IF_EXISTS;
  using std::atomic_compare_exchange_strong_explicit VSTD_USING_IF_EXISTS;
  using std::atomic_compare_exchange_weak VSTD_USING_IF_EXISTS;
  using std::atomic_compare_exchange_weak_explicit VSTD_USING_IF_EXISTS;
  using std::atomic_exchange VSTD_USING_IF_EXISTS;
  using std::atomic_exchange_explicit VSTD_USING_IF_EXISTS;
  using std::atomic_is_lock_free VSTD_USING_IF_EXISTS;
  using std::atomic_load VSTD_USING_IF_EXISTS;
  using std::atomic_load_explicit VSTD_USING_IF_EXISTS;
  using std::atomic_store VSTD_USING_IF_EXISTS;
  using std::atomic_store_explicit VSTD_USING_IF_EXISTS;

  using std::atomic_fetch_add VSTD_USING_IF_EXISTS;
  using std::atomic_fetch_add_explicit VSTD_USING_IF_EXISTS;
  using std::atomic_fetch_and VSTD_USING_IF_EXISTS;
  using std::atomic_fetch_and_explicit VSTD_USING_IF_EXISTS;
  using std::atomic_fetch_or VSTD_USING_IF_EXISTS;
  using std::atomic_fetch_or_explicit VSTD_USING_IF_EXISTS;
  using std::atomic_fetch_sub VSTD_USING_IF_EXISTS;
  using std::atomic_fetch_sub_explicit VSTD_USING_IF_EXISTS;
  using std::atomic_fetch_xor VSTD_USING_IF_EXISTS;
  using std::atomic_fetch_xor_explicit VSTD_USING_IF_EXISTS;
  using std::atomic_notify_all VSTD_USING_IF_EXISTS;
  using std::atomic_notify_one VSTD_USING_IF_EXISTS;
  using std::atomic_wait VSTD_USING_IF_EXISTS;
  using std::atomic_wait_explicit VSTD_USING_IF_EXISTS;

  // [atomics.alias], type aliases
  using std::atomic_bool VSTD_USING_IF_EXISTS;
  using std::atomic_char VSTD_USING_IF_EXISTS;
  using std::atomic_char16_t VSTD_USING_IF_EXISTS;
  using std::atomic_char32_t VSTD_USING_IF_EXISTS;
#if VSTD_HAS_CHAR8_T
  using std::atomic_char8_t VSTD_USING_IF_EXISTS;
#endif
  using std::atomic_int VSTD_USING_IF_EXISTS;
  using std::atomic_llong VSTD_USING_IF_EXISTS;
  using std::atomic_long VSTD_USING_IF_EXISTS;
  using std::atomic_schar VSTD_USING_IF_EXISTS;
  using std::atomic_short VSTD_USING_IF_EXISTS;
  using std::atomic_uchar VSTD_USING_IF_EXISTS;
  using std::atomic_uint VSTD_USING_IF_EXISTS;
  using std::atomic_ullong VSTD_USING_IF_EXISTS;
  using std::atomic_ulong VSTD_USING_IF_EXISTS;
  using std::atomic_ushort VSTD_USING_IF_EXISTS;
#if VSTD_HAS_WIDE_CHARACTERS
  using std::atomic_wchar_t VSTD_USING_IF_EXISTS;
#endif

  using std::atomic_int16_t VSTD_USING_IF_EXISTS;
  using std::atomic_int32_t VSTD_USING_IF_EXISTS;
  using std::atomic_int64_t VSTD_USING_IF_EXISTS;
  using std::atomic_int8_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint16_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint32_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint64_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint8_t VSTD_USING_IF_EXISTS;

  using std::atomic_int_least16_t VSTD_USING_IF_EXISTS;
  using std::atomic_int_least32_t VSTD_USING_IF_EXISTS;
  using std::atomic_int_least64_t VSTD_USING_IF_EXISTS;
  using std::atomic_int_least8_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint_least16_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint_least32_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint_least64_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint_least8_t VSTD_USING_IF_EXISTS;

  using std::atomic_int_fast16_t VSTD_USING_IF_EXISTS;
  using std::atomic_int_fast32_t VSTD_USING_IF_EXISTS;
  using std::atomic_int_fast64_t VSTD_USING_IF_EXISTS;
  using std::atomic_int_fast8_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint_fast16_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint_fast32_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint_fast64_t VSTD_USING_IF_EXISTS;
  using std::atomic_uint_fast8_t VSTD_USING_IF_EXISTS;

  using std::atomic_intmax_t VSTD_USING_IF_EXISTS;
  using std::atomic_intptr_t VSTD_USING_IF_EXISTS;
  using std::atomic_ptrdiff_t VSTD_USING_IF_EXISTS;
  using std::atomic_size_t VSTD_USING_IF_EXISTS;
  using std::atomic_uintmax_t VSTD_USING_IF_EXISTS;
  using std::atomic_uintptr_t VSTD_USING_IF_EXISTS;

#ifndef VSTD_NO_LOCK_FREE_TYPES
  using std::atomic_signed_lock_free VSTD_USING_IF_EXISTS;
  using std::atomic_unsigned_lock_free VSTD_USING_IF_EXISTS;
#endif

  // [atomics.flag], flag type and operations
  using std::atomic_flag VSTD_USING_IF_EXISTS;

  using std::atomic_flag_clear VSTD_USING_IF_EXISTS;
  using std::atomic_flag_clear_explicit VSTD_USING_IF_EXISTS;
  using std::atomic_flag_test VSTD_USING_IF_EXISTS;
  using std::atomic_flag_test_and_set VSTD_USING_IF_EXISTS;
  using std::atomic_flag_test_and_set_explicit VSTD_USING_IF_EXISTS;
  using std::atomic_flag_test_explicit VSTD_USING_IF_EXISTS;

  using std::atomic_flag_notify_all VSTD_USING_IF_EXISTS;
  using std::atomic_flag_notify_one VSTD_USING_IF_EXISTS;
  using std::atomic_flag_wait VSTD_USING_IF_EXISTS;
  using std::atomic_flag_wait_explicit VSTD_USING_IF_EXISTS;

  // [atomics.fences], fences
  using std::atomic_signal_fence VSTD_USING_IF_EXISTS;
  using std::atomic_thread_fence VSTD_USING_IF_EXISTS;

  // [depr.atomics.nonmembers]
  using std::atomic_init VSTD_USING_IF_EXISTS;

} // namespace std
