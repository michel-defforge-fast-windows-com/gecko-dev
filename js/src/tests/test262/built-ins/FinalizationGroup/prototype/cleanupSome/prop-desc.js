// |reftest| skip-if(!this.hasOwnProperty('FinalizationGroup')) -- FinalizationGroup is not enabled unconditionally
// Copyright (C) 2019 Leo Balter. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-finalization-group.prototype.cleanupSome
description: >
  Property descriptor of FinalizationGroup.prototype.cleanupSome
info: |
  17 ECMAScript Standard Built-in Objects:

  Every other data property described in clauses 18 through 26 and in Annex B.2
  has the attributes { [[Writable]]: true, [[Enumerable]]: false,
  [[Configurable]]: true } unless otherwise specified.
includes: [propertyHelper.js]
features: [FinalizationGroup]
---*/

assert.sameValue(typeof FinalizationGroup.prototype.cleanupSome, 'function');

verifyProperty(FinalizationGroup.prototype, 'cleanupSome', {
  enumerable: false,
  writable: true,
  configurable: true
});

reportCompare(0, 0);
