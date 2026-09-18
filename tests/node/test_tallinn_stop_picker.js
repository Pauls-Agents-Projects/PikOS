"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const picker = require("../../tools/tallinn_stop_picker.js");
const source = fs.readFileSync(
  path.join(__dirname, "../../include/WebUi.h"), "utf8"
);

const payload = [
  "ID;SiriID;Lat;Lng;Stops;Name;Info;Street;Area;City;Version",
  "a10801-1;;5944109;2473788;10801-1,10802-1;Balti jaam;0;0;Tallinna linn, Põhja-Tallinn;Harju maakond;test",
  "10801-1;1234;5944109;2473788;10009-1,21106-1;Balti jaam (train station);;Toompuiestee;Tallinna linn, Põhja-Tallinn;Harju maakond;test",
  "10802-1;1235;5944160;2473275;10403-1,21106-1;;;;;test",
  "07102-1;1470;5941050;2473147;07103-1;Tondi;;Tondi;Tallinna linn, Kristiine;Harju maakond;test",
  "bad;not a siri;5941050;2473147;;Ignored;;;;;test",
].join("\n");

const stops = picker.parseStopsPayload(payload);
assert.equal(stops.length, 3);
assert.deepEqual(stops.map((item) => item.siri_id), ["1234", "1235", "1470"]);
assert.equal(stops[1].name, "Balti jaam (train station)");
assert.equal(stops[1].area, "Tallinna linn, Põhja-Tallinn");
assert.equal(stops[0].latitude, 59.44109);
assert.equal(stops[0].longitude, 24.73788);

assert.equal(picker.searchStops(stops, "Balti", 20)[0].siri_id, "1234");
assert.equal(picker.searchStops(stops, "10802-1", 20)[0].siri_id, "1235");
assert.equal(picker.searchStops(stops, "1235", 20)[0].local_id, "10802-1");
assert.equal(picker.searchStops(stops, "Kristiine Tondi", 20)[0].siri_id, "1470");
assert.deepEqual(picker.searchStops(stops, "x", 20), []);

const channel = "0123456789abcdef0123456789abcdef";
const srcdoc = picker.makeSandboxSrcdoc(channel);
assert.match(srcdoc, /transport\.tallinn\.ee\/data\/regional_data\.js/);
assert.match(source, /buildTravelList\(\);openTallinnPicker\(index\)/);
assert.doesNotMatch(srcdoc, /allow-same-origin/);
assert.throws(() => picker.makeSandboxSrcdoc("bad"), /channel/);
assert.throws(
  () => picker.makeSandboxSrcdoc(channel, "https://example.test/stops.js"),
  /untrusted/
);

const attributes = {};
const frameElement = {setAttribute(name, value) { attributes[name] = value; }};
const created = picker.createSandboxFrame(
  {createElement(tag) { assert.equal(tag, "iframe"); return frameElement; }},
  channel
);
assert.equal(created, frameElement);
assert.equal(attributes.sandbox, "allow-scripts");
assert.equal(attributes.referrerpolicy, "no-referrer");
assert.equal(created.hidden, true);
assert.match(created.srcdoc, /regional_data\.js/);

const query = picker.makeQuery(channel, " Balti\njaam ", "request-1", 99);
assert.equal(query.query, "Balti jaam");
assert.equal(query.limit, picker.MAX_RESULTS);

const fakeWindow = {};
const frame = {contentWindow: fakeWindow};
const accepted = picker.acceptMessage({
  source: fakeWindow,
  data: {
    type: picker.RESULTS_TYPE,
    channel,
    items: [stops[0], {name: "Bad", siri_id: "not a siri"}],
  },
}, frame, channel);
assert.equal(accepted.items.length, 1);
assert.equal(picker.acceptMessage({source: {}, data: accepted}, frame, channel), null);

console.log("Tallinn stop picker tests passed");
