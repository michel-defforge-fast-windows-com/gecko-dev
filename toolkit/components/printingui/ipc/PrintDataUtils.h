/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set sw=2 ts=8 et tw=80 : */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_embedding_PrintDataUtils_h
#define mozilla_embedding_PrintDataUtils_h

#include "mozilla/embedding/PPrinting.h"
#include "nsIWebBrowserPrint.h"

/**
 * nsIPrintSettings and nsIWebBrowserPrint information is sent back and forth
 * across PPrinting via the PrintData struct. These are utilities for
 * manipulating PrintData that can be used on either side of the communications
 * channel.
 */

namespace mozilla {
namespace embedding {

class MockWebBrowserPrint final : public nsIWebBrowserPrint {
 public:
  explicit MockWebBrowserPrint(const nsAString& aDocName,
                               bool aIsIFrameSelected, bool aIsRangeSelection);

  NS_DECL_ISUPPORTS
  NS_DECL_NSIWEBBROWSERPRINT

 private:
  ~MockWebBrowserPrint();

  nsAutoString mDocName;
  bool mIsIFrameSelected;
  bool mIsRangeSelection;
};

}  // namespace embedding
}  // namespace mozilla

#endif
