/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*- */
/* vim: set ts=2 et sw=2 tw=80: */
/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _HYBRIDSDPPARSER_H_
#define _HYBRIDSDPPARSER_H_

#include "signaling/src/sdp/SdpParser.h"
#include "signaling/src/sdp/SdpTelemetry.h"

namespace mozilla {

// This shim parser delegates parsing to WEbRTC-SDP and SIPCC, based on
// preference flags. Additionally it handles collecting telemetry and fallback
// behavior between the parsers.
class HybridSdpParser : public SdpParser {
 public:
  HybridSdpParser();
  virtual ~HybridSdpParser() = default;

  auto Name() const -> const std::string& override { return PARSER_NAME; }
  auto Parse(const std::string& aText)
      -> UniquePtr<SdpParser::Results> override;

 private:
  const UniquePtr<SdpParser> mPrimary;
  const Maybe<UniquePtr<SdpParser>> mSecondary;
  const Maybe<UniquePtr<SdpParser>> mFailover;
  static const std::string PARSER_NAME;
};

}  // namespace mozilla

#endif