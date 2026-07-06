#!/usr/bin/env node
'use strict';

// Tokenizer-aware JavaScript comment stripper.
//
// A regex-based strip would corrupt the DOM shim: "//" appears inside string
// literals (e.g. namespace URIs like http://www.w3.org/...) and regex literals.
// This scanner tracks string, template-literal (including ${...} expressions),
// and regex-literal context so only genuine comments are removed.

function stripComments(src) {
  let out = '';
  let i = 0;
  const n = src.length;
  let last = ''; // last significant (non-space) emitted char, for regex detection
  let braceDepth = 0;
  const templateReturn = []; // brace depths at which a '}' resumes a template literal

  // Consume a template literal starting at i (src[i-1] was the opening char or
  // we are resuming after a ${...}). Copies through to the closing backtick, or
  // breaks into expression mode on ${.
  const scanTemplate = () => {
    while (i < n) {
      const c = src[i];
      if (c === '\\') {
        out += c + (src[i + 1] ?? '');
        i += 2;
        continue;
      }
      if (c === '`') {
        out += c;
        i += 1;
        return false; // template finished
      }
      if (c === '$' && src[i + 1] === '{') {
        out += '${';
        i += 2;
        templateReturn.push(braceDepth);
        braceDepth += 1;
        return true; // entered expression mode
      }
      out += c;
      i += 1;
    }
    return false;
  };

  while (i < n) {
    const ch = src[i];
    const next = src[i + 1];

    if (ch === '/' && next === '/') {
      i += 2;
      while (i < n && src[i] !== '\n') i += 1; // keep the newline
      continue;
    }
    if (ch === '/' && next === '*') {
      i += 2;
      while (i < n && !(src[i] === '*' && src[i + 1] === '/')) i += 1;
      i += 2;
      out += ' '; // avoid gluing adjacent tokens
      continue;
    }

    if (ch === '"' || ch === "'") {
      const quote = ch;
      out += ch;
      i += 1;
      while (i < n) {
        const c = src[i];
        out += c;
        if (c === '\\') {
          out += src[i + 1] ?? '';
          i += 2;
          continue;
        }
        i += 1;
        if (c === quote) break;
      }
      last = quote;
      continue;
    }

    if (ch === '`') {
      out += ch;
      i += 1;
      scanTemplate();
      last = '`';
      continue;
    }

    if (ch === '{') {
      braceDepth += 1;
      out += ch;
      last = ch;
      i += 1;
      continue;
    }
    if (ch === '}') {
      if (templateReturn.length > 0 && braceDepth - 1 === templateReturn[templateReturn.length - 1]) {
        templateReturn.pop();
        braceDepth -= 1;
        out += '}';
        i += 1;
        scanTemplate(); // resume the surrounding template literal
        last = '`';
        continue;
      }
      braceDepth -= 1;
      out += ch;
      last = ch;
      i += 1;
      continue;
    }

    if (ch === '/') {
      // A '/' is a regex literal unless it follows a value (identifier, number,
      // or a closing bracket), in which case it is division.
      const regexAllowed = last === '' || !/[\w$)\]}]/.test(last);
      if (regexAllowed) {
        out += ch;
        i += 1;
        let inClass = false;
        while (i < n) {
          const c = src[i];
          out += c;
          if (c === '\\') {
            out += src[i + 1] ?? '';
            i += 2;
            continue;
          }
          if (c === '[') inClass = true;
          else if (c === ']') inClass = false;
          i += 1;
          if (c === '/' && !inClass) break;
        }
        last = '/';
        continue;
      }
      out += ch;
      last = '/';
      i += 1;
      continue;
    }

    out += ch;
    if (!/\s/.test(ch)) last = ch;
    i += 1;
  }

  return out;
}

function main() {
  const input = process.argv[2];
  const output = process.argv[3] || input;
  if (!input) {
    process.stderr.write('Usage: strip_js_comments.js <input.js> [output.js]\n');
    process.exit(2);
  }
  const fs = require('fs');
  const source = fs.readFileSync(input, 'utf8');
  const stripped = stripComments(source);

  // The regex-vs-division heuristic above cannot be perfect; guard against it
  // corrupting the source by re-parsing the stripped output. vm.Script only
  // compiles (parses) — it never runs the code — so a syntax error here means the
  // stripper broke a regex literal / string / template and the build must fail
  // loudly instead of shipping broken JS the shim would only reject at runtime.
  try {
    // eslint-disable-next-line no-new
    new (require('vm').Script)(stripped, { filename: output });
  } catch (error) {
    process.stderr.write(
      'strip_js_comments produced invalid JS for ' + input + ': ' + error.message + '\n');
    process.exit(1);
  }

  fs.writeFileSync(output, stripped);
}

main();
