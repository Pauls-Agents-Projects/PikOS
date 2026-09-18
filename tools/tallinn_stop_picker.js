/* Browser-only Tallinn stop search for PikOS's settings UI.
 *
 * The clock must never download the ~566 KiB regional stop catalogue. A
 * sandboxed, opaque-origin iframe loads Tallinn's first-party script directly,
 * retains the catalogue in browser memory, and posts only bounded search
 * results to the settings page. This module is also CommonJS-compatible so
 * its parser/ranker can be tested with Node without network access.
 */
(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.PikOSTallinnStopPicker = api;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  const DATASET_URL = "https://transport.tallinn.ee/data/regional_data.js";
  const QUERY_TYPE = "pikos.tallinn-stops.query.v1";
  const READY_TYPE = "pikos.tallinn-stops.ready.v1";
  const RESULTS_TYPE = "pikos.tallinn-stops.results.v1";
  const ERROR_TYPE = "pikos.tallinn-stops.error.v1";
  const SAFE_SIRI = /^[A-Za-z0-9._-]{1,12}$/;
  const MAX_RESULTS = 20;

  function clean(value, maximum) {
    return String(value == null ? "" : value)
      .replace(/[\u0000-\u001f\u007f]/g, " ")
      .replace(/\s+/g, " ")
      .trim()
      .slice(0, maximum);
  }

  function normalise(value) {
    return clean(value, 128)
      .normalize("NFD")
      .replace(/[\u0300-\u036f]/g, "")
      .toLocaleLowerCase("et-EE");
  }

  function parseStopsPayload(payload) {
    if (typeof payload !== "string" || payload.length < 10 ||
        payload.length > 2000000) {
      throw new Error("invalid Tallinn stop catalogue");
    }
    const lines = payload.replace(/^\uFEFF/, "").split(/\r?\n/);
    const header = lines.shift().split(";").map((value) => value.trim().toUpperCase());
    const field = Object.create(null);
    header.forEach((name, index) => { field[name] = index; });
    for (const required of ["ID", "SIRIID", "LAT", "LNG", "NAME"]) {
      if (!Number.isInteger(field[required])) {
        throw new Error("unknown Tallinn stop catalogue schema");
      }
    }

    const stops = [];
    const seen = new Set();
    let inheritedName = "";
    let inheritedArea = "";
    let inheritedStreet = "";
    let inheritedCity = "";
    for (const line of lines) {
      if (stops.length >= 20000) break;
      if (!line) continue;
      const parts = line.split(";");
      const value = (name) => parts[field[name]] || "";
      const update = (raw, previous, limit) => {
        const next = clean(raw, limit);
        if (!next) return previous;
        return next === "0" ? "" : next;
      };
      inheritedName = update(value("NAME"), inheritedName, 64);
      inheritedArea = update(value("AREA"), inheritedArea, 64);
      inheritedStreet = update(value("STREET"), inheritedStreet, 64);
      inheritedCity = update(value("CITY"), inheritedCity, 64);

      const siriId = clean(value("SIRIID"), 12);
      if (!SAFE_SIRI.test(siriId) || seen.has(siriId) || !inheritedName) continue;
      const latitude = Number(value("LAT")) / 100000;
      const longitude = Number(value("LNG")) / 100000;
      const stop = {
        name: inheritedName,
        siri_id: siriId,
        local_id: clean(value("ID"), 24),
        area: inheritedArea,
        street: inheritedStreet,
        city: inheritedCity,
      };
      if (Number.isFinite(latitude) && latitude >= -90 && latitude <= 90) {
        stop.latitude = latitude;
      }
      if (Number.isFinite(longitude) && longitude >= -180 && longitude <= 180) {
        stop.longitude = longitude;
      }
      seen.add(siriId);
      stops.push(stop);
    }
    return stops;
  }

  function searchStops(stops, query, requestedLimit) {
    const needle = normalise(query);
    if (needle.length < 2 || !Array.isArray(stops)) return [];
    const limit = Math.max(1, Math.min(MAX_RESULTS, Number(requestedLimit) || MAX_RESULTS));
    const words = needle.split(/\s+/).filter(Boolean);
    const ranked = [];
    for (const stop of stops) {
      if (!stop || !SAFE_SIRI.test(String(stop.siri_id || ""))) continue;
      const name = normalise(stop.name);
      const localId = normalise(stop.local_id);
      const siri = normalise(stop.siri_id);
      const details = normalise([stop.street, stop.area, stop.city].filter(Boolean).join(" "));
      const whole = `${name} ${details} ${localId} ${siri}`;
      if (!words.every((word) => whole.includes(word))) continue;
      let score = 80;
      if (siri === needle) score = 0;
      else if (localId === needle) score = 1;
      else if (name === needle) score = 2;
      else if (siri.startsWith(needle)) score = 5;
      else if (localId.startsWith(needle)) score = 6;
      else if (name.startsWith(needle)) score = 10;
      else if (name.split(/\s+/).some((word) => word.startsWith(needle))) score = 20;
      else if (name.includes(needle)) score = 30;
      else if (details.includes(needle)) score = 50;
      ranked.push({score, stop});
    }
    ranked.sort((left, right) =>
      left.score - right.score ||
      String(left.stop.name).localeCompare(String(right.stop.name), "et-EE") ||
      String(left.stop.siri_id).localeCompare(String(right.stop.siri_id))
    );
    return ranked.slice(0, limit).map(({stop}) => ({...stop}));
  }

  function validChannel(channel) {
    return /^[0-9a-f]{32}$/i.test(String(channel || ""));
  }

  function makeSandboxSrcdoc(channel, datasetUrl) {
    if (!validChannel(channel)) throw new Error("invalid picker channel");
    const url = String(datasetUrl || DATASET_URL);
    if (!/^https:\/\/transport\.tallinn\.ee\/[A-Za-z0-9_?&=./-]+$/.test(url)) {
      throw new Error("untrusted Tallinn stop catalogue URL");
    }
    const runtime = `(() => {
      "use strict";
      const CHANNEL=${JSON.stringify(channel)};
      const QUERY_TYPE=${JSON.stringify(QUERY_TYPE)};
      const READY_TYPE=${JSON.stringify(READY_TYPE)};
      const RESULTS_TYPE=${JSON.stringify(RESULTS_TYPE)};
      const ERROR_TYPE=${JSON.stringify(ERROR_TYPE)};
      const SAFE_SIRI=${SAFE_SIRI.toString()};
      const MAX_RESULTS=${MAX_RESULTS};
      const clean=${clean.toString()};
      const normalise=${normalise.toString()};
      const parseStopsPayload=${parseStopsPayload.toString()};
      const searchStops=${searchStops.toString()};
      let stops=[];
      const post=(message) => parent.postMessage(Object.assign({channel:CHANNEL}, message), "*");
      window.ti={
        loadStops(payload) {
          try {
            stops=parseStopsPayload(payload);
            post({type:READY_TYPE,count:stops.length});
          } catch (_) {
            stops=[];
            post({type:ERROR_TYPE,error:"Tallinn stop catalogue could not be read"});
          }
        },
        loadRoutes() {}
      };
      addEventListener("message", (event) => {
        const data=event.data || {};
        if (event.source !== parent || data.channel !== CHANNEL || data.type !== QUERY_TYPE) return;
        post({type:RESULTS_TYPE,request_id:String(data.request_id || "").slice(0,64),
              items:searchStops(stops,String(data.query || "").slice(0,64),data.limit)});
      });
      window.pikosTallinnDatasetError=() => post({type:ERROR_TYPE,error:"Tallinn stop catalogue is unavailable"});
    })();`;
    return `<!doctype html><meta charset="utf-8"><script>${runtime}<\/script>` +
      `<script src="${url}" referrerpolicy="no-referrer" onerror="pikosTallinnDatasetError()"><\/script>`;
  }

  function createSandboxFrame(documentObject, channel, datasetUrl) {
    if (!documentObject || typeof documentObject.createElement !== "function") {
      throw new Error("document is required");
    }
    const frame = documentObject.createElement("iframe");
    frame.hidden = true;
    frame.setAttribute("sandbox", "allow-scripts");
    frame.setAttribute("title", "Tallinn stop search data");
    frame.setAttribute("referrerpolicy", "no-referrer");
    frame.srcdoc = makeSandboxSrcdoc(channel, datasetUrl);
    return frame;
  }

  function makeQuery(channel, query, requestId, limit) {
    if (!validChannel(channel)) throw new Error("invalid picker channel");
    return {
      type: QUERY_TYPE,
      channel,
      query: clean(query, 64),
      request_id: clean(requestId, 64),
      limit: Math.max(1, Math.min(MAX_RESULTS, Number(limit) || MAX_RESULTS)),
    };
  }

  function acceptMessage(event, frame, channel) {
    if (!event || !frame || event.source !== frame.contentWindow) return null;
    const data = event.data;
    if (!data || data.channel !== channel ||
        ![READY_TYPE, RESULTS_TYPE, ERROR_TYPE].includes(data.type)) return null;
    if (data.type === RESULTS_TYPE) {
      const items = Array.isArray(data.items) ? data.items.slice(0, MAX_RESULTS) : [];
      data.items = items.filter((item) =>
        item && SAFE_SIRI.test(String(item.siri_id || "")) &&
        typeof item.name === "string"
      );
    }
    return data;
  }

  return {
    DATASET_URL,
    QUERY_TYPE,
    READY_TYPE,
    RESULTS_TYPE,
    ERROR_TYPE,
    MAX_RESULTS,
    parseStopsPayload,
    searchStops,
    makeSandboxSrcdoc,
    createSandboxFrame,
    makeQuery,
    acceptMessage,
  };
});
