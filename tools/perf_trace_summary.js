#!/usr/bin/env node
"use strict";

const fs = require("fs");

class Aggregate {
  constructor() {
    this.events = 0;
    this.elapsedUs = 0;
    this.maxElapsedUs = 0;
    this.count = 0;
  }

  add(event) {
    const elapsedUs = numberValue(event.elapsed_us);
    const count = numberValue(event.count);
    this.events += 1;
    this.elapsedUs += elapsedUs;
    this.maxElapsedUs = Math.max(this.maxElapsedUs, elapsedUs);
    this.count += count;
  }
}

function usage() {
  console.error("usage: tools/perf_trace_summary.js TRACE [--top NUMBER]");
  console.error("       tools/perf_trace_summary.js - [--top NUMBER] < trace.jsonl");
}

function parseArgs(argv) {
  let trace = null;
  let top = 12;

  for (let i = 2; i < argv.length; ++i) {
    const arg = argv[i];
    if (arg === "--help" || arg === "-h") {
      usage();
      process.exit(0);
    }
    if (arg === "--top") {
      if (i + 1 >= argv.length) {
        throw new Error("missing value for --top");
      }
      const value = Number(argv[++i]);
      if (!Number.isInteger(value)) {
        throw new Error("--top must be an integer");
      }
      top = Math.max(0, value);
      continue;
    }
    if (arg.startsWith("--top=")) {
      const value = Number(arg.slice("--top=".length));
      if (!Number.isInteger(value)) {
        throw new Error("--top must be an integer");
      }
      top = Math.max(0, value);
      continue;
    }
    if (arg.startsWith("-") && arg !== "-") {
      throw new Error(`unknown argument: ${arg}`);
    }
    if (trace !== null) {
      throw new Error(`unexpected extra argument: ${arg}`);
    }
    trace = arg;
  }

  if (trace === null) {
    throw new Error("missing trace path");
  }
  return { trace, top };
}

function numberValue(value) {
  const number = Number(value || 0);
  return Number.isFinite(number) ? number : 0;
}

function stringValue(value) {
  return value === undefined || value === null ? "" : String(value);
}

function ms(us) {
  return (us / 1000).toFixed(3);
}

function loadEvents(path) {
  const input = path === "-"
    ? fs.readFileSync(0, "utf8")
    : fs.readFileSync(path, "utf8");
  const events = [];

  input.split(/\r?\n/).forEach((line, index) => {
    const text = line.trim();
    if (!text) {
      return;
    }
    let event;
    try {
      event = JSON.parse(text);
    } catch (error) {
      throw new Error(`${path}:${index + 1}: invalid JSON: ${error.message}`);
    }
    if (event && typeof event === "object" && !Array.isArray(event)) {
      events.push(event);
    }
  });
  return events;
}

function getAggregate(map, key) {
  let aggregate = map.get(key);
  if (!aggregate) {
    aggregate = new Aggregate();
    map.set(key, aggregate);
  }
  return aggregate;
}

function printTable(headers, rows) {
  const widths = headers.map((header) => header.length);
  for (const row of rows) {
    row.forEach((cell, index) => {
      widths[index] = Math.max(widths[index], cell.length);
    });
  }

  console.log(headers.map((header, index) => header.padEnd(widths[index])).join("  "));
  console.log(widths.map((width) => "-".repeat(width)).join("  "));
  for (const row of rows) {
    console.log(row.map((cell, index) => cell.padEnd(widths[index])).join("  "));
  }
}

function summarize(path, top) {
  const events = loadEvents(path);
  const phases = new Map();
  const resourceGroups = new Map();

  for (const event of events) {
    getAggregate(phases, stringValue(event.phase)).add(event);
    if (event.phase === "resource_load") {
      const key = [
        stringValue(event.name),
        stringValue(event.property),
        stringValue(event.reason),
      ].join("\u0000");
      getAggregate(resourceGroups, key).add(event);
    }
  }

  console.log(`trace: ${path}`);
  console.log(`events: ${events.length}`);
  console.log();

  const phaseRows = [...phases.entries()]
    .sort((a, b) => b[1].elapsedUs - a[1].elapsedUs)
    .map(([phase, aggregate]) => [
      phase,
      String(aggregate.events),
      ms(aggregate.elapsedUs),
      ms(aggregate.maxElapsedUs),
      String(aggregate.count),
    ]);

  console.log("Phases");
  printTable(["phase", "events", "total_ms", "max_ms", "count_sum"], phaseRows);

  if (resourceGroups.size === 0) {
    return;
  }

  console.log();
  const resourceRows = [...resourceGroups.entries()]
    .sort((a, b) => {
      const elapsedDelta = b[1].elapsedUs - a[1].elapsedUs;
      return elapsedDelta !== 0 ? elapsedDelta : b[1].events - a[1].events;
    })
    .map(([key, aggregate]) => {
      const [name, property, reason] = key.split("\u0000");
      return [
        name,
        property,
        reason,
        String(aggregate.events),
        ms(aggregate.elapsedUs),
        ms(aggregate.maxElapsedUs),
        String(aggregate.count),
      ];
    });

  console.log("Resource Loads");
  printTable(["name", "property", "reason", "events", "total_ms", "max_ms", "bytes"], resourceRows);

  const slowResources = events
    .filter((event) => event.phase === "resource_load" && numberValue(event.elapsed_us) > 0)
    .sort((a, b) => numberValue(b.elapsed_us) - numberValue(a.elapsed_us));
  if (slowResources.length === 0 || top === 0) {
    return;
  }

  console.log();
  const topRows = slowResources.slice(0, top).map((event) => [
    ms(numberValue(event.elapsed_us)),
    String(numberValue(event.count)),
    stringValue(event.name),
    stringValue(event.property),
    stringValue(event.reason),
    stringValue(event.url),
  ]);
  console.log(`Top ${Math.min(top, slowResources.length)} Resource Events`);
  printTable(["ms", "bytes", "name", "property", "reason", "url"], topRows);
}

try {
  const { trace, top } = parseArgs(process.argv);
  summarize(trace, top);
} catch (error) {
  console.error(`perf_trace_summary: ${error.message}`);
  usage();
  process.exit(1);
}
