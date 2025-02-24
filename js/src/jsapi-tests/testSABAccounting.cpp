#include "jsapi.h"
#include "jsfriendapi.h"

#include "builtin/TestingFunctions.h"
#include "js/SharedArrayBuffer.h"
#include "jsapi-tests/tests.h"

BEGIN_TEST(testSABAccounting) {
  // Purge what we can
  JS::PrepareForFullGC(cx);
  NonIncrementalGC(cx, GC_SHRINK, JS::GCReason::API);

  // Self-hosting and chrome code should not use SABs, or the point of this
  // predicate is completely lost.
  CHECK(!JS_ContainsSharedArrayBuffer(cx));

  JS::RootedObject obj(cx), obj2(cx);
  CHECK(obj = JS::NewSharedArrayBuffer(cx, 4096));
  CHECK(JS_ContainsSharedArrayBuffer(cx));
  CHECK(obj2 = JS::NewSharedArrayBuffer(cx, 4096));
  CHECK(JS_ContainsSharedArrayBuffer(cx));

  // Discard those objects again.
  obj = nullptr;
  obj2 = nullptr;
  JS::PrepareForFullGC(cx);
  NonIncrementalGC(cx, GC_SHRINK, JS::GCReason::API);

  // Should be back to base state.
  CHECK(!JS_ContainsSharedArrayBuffer(cx));

  return true;
}
END_TEST(testSABAccounting)
