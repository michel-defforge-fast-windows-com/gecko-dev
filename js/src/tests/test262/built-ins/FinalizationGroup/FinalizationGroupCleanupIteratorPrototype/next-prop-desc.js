// |reftest| skip-if(!this.hasOwnProperty('FinalizationGroup')) async -- FinalizationGroup is not enabled unconditionally
// Copyright (C) 2019 Leo Balter. All rights reserved.
// This code is governed by the BSD license found in the LICENSE file.

/*---
esid: sec-properties-of-the-finalization-group-constructor
description: >
  Prop descriptor for FinalizationGroupCleanupIteratorPrototype.next
info: |
  FinalizationGroup.prototype.cleanupSome ( [ callback ] )

  1. Let finalizationGroup be the this value.
  ...
  5. Perform ! CleanupFinalizationGroup(finalizationGroup, callback).
  ...

  CleanupFinalizationGroup ( finalizationGroup [ , callback ] )

  ...
  3. Let iterator be ! CreateFinalizationGroupCleanupIterator(finalizationGroup).
  ...
  6. Let result be Call(callback, undefined, « iterator »).
  ...

  17 ECMAScript Standard Built-in Objects:

  Every other data property described in clauses 18 through 26 and in Annex B.2
  has the attributes { [[Writable]]: true, [[Enumerable]]: false,
  [[Configurable]]: true } unless otherwise specified.
includes: [propertyHelper.js, async-gc.js]
flags: [async, non-deterministic]
features: [FinalizationGroup, host-gc-required, Symbol]
---*/

var FinalizationGroupCleanupIteratorPrototype;
var called = 0;
var fg = new FinalizationGroup(function() {});

function callback(iterator) {
  called += 1;
  FinalizationGroupCleanupIteratorPrototype = Object.getPrototypeOf(iterator);
}

function emptyCells() {
  var target = {};
  fg.register(target);

  var prom = asyncGC(target);
  target = null;

  return prom;
}

emptyCells().then(function() {
  fg.cleanupSome(callback);

  assert.sameValue(called, 1, 'cleanup successful');

  assert.sameValue(typeof FinalizationGroupCleanupIteratorPrototype.next, 'function');

  var next = FinalizationGroupCleanupIteratorPrototype.next;

  verifyProperty(FinalizationGroupCleanupIteratorPrototype, 'next', {
    enumerable: false,
    writable: true,
    configurable: true,
  });
}).then($DONE, resolveAsyncGC);
