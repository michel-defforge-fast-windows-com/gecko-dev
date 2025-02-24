/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=8 sts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_indexeddb_initializedonce_h__
#define mozilla_dom_indexeddb_initializedonce_h__

#include "mozilla/Assertions.h"
#include "mozilla/Maybe.h"

#include <type_traits>

// This file is not exported and is only meant to be included in IndexedDB
// source files.

// TODO: However, it is meant to be moved to mfbt, and therefore already placed
// in the mozilla namespace.

namespace mozilla {

enum struct LazyInit { Allow, Forbid };

// A kind of mozilla::Maybe that can only be initialized and cleared once. It
// cannot be re-initialized. This is a more stateful than a const Maybe<T> in
// that it can be cleared, but much less stateful than a non-const Maybe<T>
// which could be reinitialized multiple times. Use with a const T to ensure
// that the contents cannot be modified either.
template <typename T, LazyInit LazyInit = LazyInit::Forbid>
class InitializedOnce final {
 public:
  template <typename Dummy = void>
  explicit InitializedOnce(
      std::enable_if_t<LazyInit == LazyInit::Allow, Dummy>* = nullptr) {}

  template <typename... Args>
  explicit InitializedOnce(Args&&... aArgs)
      : mMaybe{Some(T{std::forward<Args>(aArgs)...})} {}

  InitializedOnce& operator=(const InitializedOnce&) = delete;
  InitializedOnce& operator=(InitializedOnce&&) = delete;

  template <typename... Args, typename Dummy = void>
  std::enable_if_t<LazyInit == LazyInit::Allow, Dummy> init(Args&&... aArgs) {
    MOZ_ASSERT(mMaybe.isNothing());
    MOZ_ASSERT(!mWasReset);
    mMaybe.emplace(T{std::forward<Args>(aArgs)...});
  }

  explicit operator bool() const { return isSome(); }
  bool isSome() const { return mMaybe.isSome(); }
  bool isNothing() const { return mMaybe.isNothing(); }

  T& operator*() { return *mMaybe; }
  T* operator->() { return mMaybe.operator->(); }

  std::add_const_t<T>& operator*() const { return *mMaybe; }
  std::add_const_t<T>* operator->() const { return mMaybe.operator->(); }

  void reset() {
    MOZ_ASSERT(mMaybe.isSome());
    mMaybe.reset();
#ifdef DEBUG
    mWasReset = true;
#endif
  }

 private:
  Maybe<T> mMaybe;
#ifdef DEBUG
  bool mWasReset = false;
#endif
};

}  // namespace mozilla

#endif
