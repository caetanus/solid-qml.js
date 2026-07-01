import { test } from "node:test";
import assert from "node:assert/strict";
import { safeName } from "../src/names/safe.ts";

test("safeName: passes through a normal identifier", () => {
  assert.equal(safeName("count"), "count");
  assert.equal(safeName("myDiv"), "myDiv");
});

test("safeName: escapes QML reserved words by appending _", () => {
  assert.equal(safeName("visible"), "visible_");
  assert.equal(safeName("width"), "width_");
  assert.equal(safeName("state"), "state_");
  assert.equal(safeName("color"), "color_");
});

test("safeName: escapes a leading-digit or invalid identifier", () => {
  assert.equal(safeName("2foo"), "_2foo");
});
