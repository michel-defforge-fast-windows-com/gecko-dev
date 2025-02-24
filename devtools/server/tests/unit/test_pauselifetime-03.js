/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */
/* eslint-disable no-shadow, max-nested-callbacks */

"use strict";

/**
 * Check that pause-lifetime grip clients are marked invalid after a resume.
 */

var gDebuggee;
var gClient;
var gThreadFront;

add_task(
  threadFrontTest(
    async ({ threadFront, debuggee, client }) => {
      gThreadFront = threadFront;
      gDebuggee = debuggee;
      gClient = client;
      test_pause_frame();
    },
    { waitForFinish: true }
  )
);

function test_pause_frame() {
  gThreadFront.once("paused", async function(packet) {
    const args = packet.frame.arguments;
    const objActor = args[0].actor;
    Assert.equal(args[0].class, "Object");
    Assert.ok(!!objActor);

    const objectFront = gThreadFront.pauseGrip(args[0]);
    Assert.ok(objectFront.valid);

    // Make a bogus request to the grip actor.  Should get
    // unrecognized-packet-type (and not no-such-actor).
    try {
      const objFront = gClient.getFrontByID(objActor);
      await objFront.request({ to: objActor, type: "bogusRequest" });
      ok(false, "bogusRequest should throw");
    } catch (e) {
      ok(true, "bogusRequest thrown");
      Assert.ok(!!e.match(/unrecognizedPacketType/));
    }
    Assert.ok(objectFront.valid);

    gThreadFront.resume().then(async function() {
      // Now that we've resumed, should get no-such-actor for the
      // same request.
      try {
        const objFront = gClient.getFrontByID(objActor);
        await objFront.request({ to: objActor, type: "bogusRequest" });
        ok(false, "bogusRequest should throw");
      } catch (e) {
        ok(true, "bogusRequest thrown");
        Assert.ok(!!e.match(/noSuchActor/));
      }
      Assert.ok(!objectFront.valid);
      threadFrontTestFinished();
    });
  });

  gDebuggee.eval(
    "(" +
      function() {
        function stopMe(obj) {
          debugger;
        }
        stopMe({ foo: "bar" });
      } +
      ")()"
  );
}
