/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */
/* eslint-disable no-shadow, max-nested-callbacks */

"use strict";

var gDebuggee;
var gThreadFront;

// Test that closures can be inspected.

add_task(
  threadFrontTest(
    async ({ threadFront, debuggee }) => {
      gThreadFront = threadFront;
      gDebuggee = debuggee;
      test_object_grip();
    },
    { waitForFinish: true }
  )
);

function test_object_grip() {
  gThreadFront.once("paused", async function(packet) {
    const environment = await packet.frame.getEnvironment();
    const person = environment.bindings.variables.person;

    Assert.equal(person.value.class, "Object");

    const personFront = gThreadFront.pauseGrip(person.value);
    const { ownProperties } = await personFront.getPrototypeAndProperties();
    Assert.equal(ownProperties.getName.value.getGrip().class, "Function");
    Assert.equal(ownProperties.getAge.value.getGrip().class, "Function");
    Assert.equal(ownProperties.getFoo.value.getGrip().class, "Function");

    const getNameFront = ownProperties.getName.value;
    const getAgeFront = ownProperties.getAge.value;
    const getFooFront = ownProperties.getFoo.value;

    let response = await getNameFront.getScope();
    let bindings = await response.scope.bindings();
    Assert.equal(bindings.arguments[0].name.value, "Bob");

    response = await getAgeFront.getScope();
    bindings = await response.scope.bindings();
    Assert.equal(bindings.arguments[1].age.value, 58);

    response = await getFooFront.getScope();
    bindings = await response.scope.bindings();
    Assert.equal(bindings.variables.foo.value, 10);

    await gThreadFront.resume();
    threadFrontTestFinished();
  });

  /* eslint-disable */
  gDebuggee.eval(
    "(" +
      function() {
        var PersonFactory = function(name, age) {
          var foo = 10;
          return {
            getName: function() {
              return name;
            },
            getAge: function() {
              return age;
            },
            getFoo: function() {
              foo = Date.now();
              return foo;
            },
          };
        };
        var person = new PersonFactory("Bob", 58);
        debugger;
      } +
      ")()"
  );
  /* eslint-enable */
}
