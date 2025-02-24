/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */
/* eslint-disable no-shadow, max-nested-callbacks */

"use strict";

/**
 * Check the environment bindings of a |with| in global scope.
 */

var gDebuggee;
var gThreadFront;

add_task(
  threadFrontTest(
    async ({ threadFront, debuggee }) => {
      gThreadFront = threadFront;
      gDebuggee = debuggee;
      test_pause_frame();
    },
    { waitForFinish: true }
  )
);

function test_pause_frame() {
  gThreadFront.once("paused", async function(packet) {
    const env = await packet.frame.getEnvironment();
    Assert.notEqual(env, undefined);

    const objClient = gThreadFront.pauseGrip(env.object);
    let response = await objClient.getPrototypeAndProperties();
    Assert.equal(response.ownProperties.PI.value, Math.PI);
    Assert.equal(response.ownProperties.cos.value.getGrip().type, "object");
    Assert.equal(response.ownProperties.cos.value.getGrip().class, "Function");
    Assert.ok(!!response.ownProperties.cos.value.actorID);

    // Skip the global lexical scope.
    const parentEnv = env.parent.parent;
    Assert.notEqual(parentEnv, undefined);

    const parentClient = gThreadFront.pauseGrip(parentEnv.object);
    response = await parentClient.getPrototypeAndProperties();
    Assert.equal(response.ownProperties.a.value, Math.PI * 100);
    Assert.equal(response.ownProperties.r.value, 10);
    Assert.equal(response.ownProperties.Object.value.getGrip().type, "object");
    Assert.equal(
      response.ownProperties.Object.value.getGrip().class,
      "Function"
    );
    Assert.ok(!!response.ownProperties.Object.value.actorID);

    await gThreadFront.resume();
    threadFrontTestFinished();
  });

  gDebuggee.eval(
    "var a, r = 10;\n" +
      "with (Math) {\n" +
      "  a = PI * r * r;\n" +
      "  debugger;\n" +
      "}"
  );
}
