#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');

const SUPPORTED_EXTENSIONS = ['.window.js', '.any.js', '.html'];
const DEFAULT_REQUIRES_RENDERING_PREFIXES = ['css/cssom-view/'];

function usage() {
  console.error([
    'usage: generate_wpt_manifest.js --root PATH --prefix PREFIX [--prefix PREFIX...] --output PATH [options]',
    '',
    'Options:',
    '  --limit N                         keep at most N matching tests after sorting',
    '  --include-unsupported             disable source-content filters for unsupported WPT infrastructure',
    '  --requires-rendering-prefix PATH   mark matching tests as requiresRendering (repeatable)',
    '',
    'Example:',
    '  node tools/generate_wpt_manifest.js \\',
    '    --root /path/to/wpt \\',
    '    --prefix url/ --prefix dom/nodes/ --prefix css/cssom-view/ \\',
    '    --output /tmp/pagecore-wpt-subset.json'
  ].join('\n'));
}

function parseArgs(argv) {
  const args = {
    prefixes: [],
    requiresRenderingPrefixes: [...DEFAULT_REQUIRES_RENDERING_PREFIXES],
    includeUnsupported: false,
    limit: null
  };

  for (let i = 2; i < argv.length; i++) {
    const arg = argv[i];
    const next = () => {
      if (i + 1 >= argv.length) throw new Error(`missing value for ${arg}`);
      return argv[++i];
    };

    if (arg === '--root') args.root = next();
    else if (arg === '--prefix') args.prefixes.push(next());
    else if (arg === '--output') args.output = next();
    else if (arg === '--limit') args.limit = Number(next());
    else if (arg === '--include-unsupported') args.includeUnsupported = true;
    else if (arg === '--requires-rendering-prefix') args.requiresRenderingPrefixes.push(next());
    else if (arg === '--help' || arg === '-h') {
      usage();
      process.exit(0);
    } else {
      throw new Error(`unknown argument: ${arg}`);
    }
  }

  if (!args.root) throw new Error('--root is required');
  if (!args.output) throw new Error('--output is required');
  if (args.prefixes.length === 0) throw new Error('at least one --prefix is required');
  if (args.limit !== null && (!Number.isInteger(args.limit) || args.limit <= 0)) {
    throw new Error('--limit must be a positive integer');
  }

  args.prefixes = args.prefixes.map(normalizePrefix);
  args.requiresRenderingPrefixes = args.requiresRenderingPrefixes.map(normalizePrefix);
  return args;
}

function normalizeSlashes(value) {
  return String(value).replace(/\\/g, '/');
}

function normalizePrefix(value) {
  let prefix = normalizeSlashes(value).replace(/^\/+/, '');
  if (prefix !== '' && !prefix.endsWith('/')) prefix += '/';
  return prefix;
}

function pathToManifestPath(relativePath) {
  return `/${normalizeSlashes(relativePath).replace(/^\/+/, '')}`;
}

function manifestName(relativePath) {
  const normalized = normalizeSlashes(relativePath).replace(/^\/+/, '');
  for (const suffix of ['.window.js', '.any.js', '.html']) {
    if (normalized.endsWith(suffix)) {
      return normalized.slice(0, -suffix.length);
    }
  }
  return normalized;
}

function isSupportedExtension(relativePath) {
  return SUPPORTED_EXTENSIONS.some((suffix) => relativePath.endsWith(suffix));
}

function pathSkipReason(relativePath) {
  const normalized = normalizeSlashes(relativePath);
  const segments = normalized.split('/');
  const basename = segments[segments.length - 1];

  if (!isSupportedExtension(normalized)) return 'unsupported extension';
  if (segments.includes('resources') || segments.includes('support')) return 'support resource';
  if (basename.endsWith('-ref.html') || basename.endsWith('-reference.html')) return 'reference file';
  if (basename.includes('.worker.') || basename.includes('.serviceworker.') || basename.includes('.sharedworker.')) {
    return 'worker test';
  }
  if (basename.includes('.sub.')) return 'wptserve template';
  if (basename.endsWith('.manual.html')) return 'manual test';
  return null;
}

function sourceSkipReason(source) {
  const checks = [
    [/\.py(?:[?#'")]|$)/, 'python handler'],
    [/\{\{[^}]+\}\}/, 'wptserve template substitution'],
    [/\bnew\s+(SharedWorker|Worker)\b|\bnavigator\.serviceWorker\b|\bserviceWorker\b/, 'worker API'],
    [/\bWebSocket\b|\bEventSource\b/, 'persistent network API'],
    [/\bget_host_info\s*\(|www\d?\.web-platform\.test|www\.web-platform\.test/, 'multi-origin helper'],
    [/\bcreateElement\s*\(\s*['"]iframe['"]\s*\)|<iframe\b/i, 'iframe navigation'],
    [/javascript:/i, 'javascript URL navigation']
  ];

  for (const [pattern, reason] of checks) {
    if (pattern.test(source)) return reason;
  }
  return null;
}

function scriptSourceReferences(source) {
  const references = [];
  const scriptPattern = /<script\b[^>]*\bsrc\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))/gi;
  let match;
  while ((match = scriptPattern.exec(source)) !== null) {
    references.push({ kind: 'script', value: match[1] || match[2] || match[3] || '' });
  }
  return references;
}

function attributeValue(tag, name) {
  const pattern = new RegExp(`\\b${name}\\s*=\\s*(?:"([^"]*)"|'([^']*)'|([^\\s>]+))`, 'i');
  const match = pattern.exec(tag);
  return match ? (match[1] || match[2] || match[3] || '') : '';
}

function localHtmlResourceReferences(source) {
  const references = scriptSourceReferences(source);
  const linkPattern = /<link\b[^>]*>/gi;
  let match;
  while ((match = linkPattern.exec(source)) !== null) {
    const tag = match[0];
    const rel = attributeValue(tag, 'rel').toLowerCase().split(/\s+/);
    if (!rel.includes('stylesheet')) continue;
    references.push({ kind: 'stylesheet', value: attributeValue(tag, 'href') });
  }
  return references;
}

function dependencyPathForHtmlResource(relativePath, src) {
  const raw = String(src || '').trim();
  if (!raw) return null;
  if (/^(?:[a-z][a-z0-9+.-]*:)?\/\//i.test(raw)) return null;
  if (/^(?:data|javascript|mailto):/i.test(raw)) return null;

  const pathOnly = raw.split(/[?#]/, 1)[0];
  if (!pathOnly) return null;
  if (pathOnly.startsWith('/')) {
    return normalizeSlashes(pathOnly.replace(/^\/+/, ''));
  }

  const base = path.posix.dirname(normalizeSlashes(relativePath));
  const resolved = path.posix.normalize(path.posix.join(base, pathOnly));
  return normalizeSlashes(resolved).replace(/^\/+/, '');
}

function missingLocalResourceReason(root, relativePath, source) {
  if (!relativePath.endsWith('.html')) return null;
  for (const reference of localHtmlResourceReferences(source)) {
    const dependency = dependencyPathForHtmlResource(relativePath, reference.value);
    if (!dependency) continue;
    if (!fs.existsSync(path.join(root, dependency))) {
      return reference.kind === 'stylesheet' ? 'missing stylesheet resource' : 'missing helper script';
    }
  }
  return null;
}

function walkFiles(root) {
  const files = [];
  const pending = [''];

  while (pending.length > 0) {
    const relativeDir = pending.pop();
    const absoluteDir = path.join(root, relativeDir);
    const entries = fs.readdirSync(absoluteDir, { withFileTypes: true });
    entries.sort((a, b) => a.name.localeCompare(b.name));

    for (const entry of entries) {
      const relativePath = relativeDir ? path.join(relativeDir, entry.name) : entry.name;
      if (entry.isDirectory()) {
        pending.push(relativePath);
      } else if (entry.isFile()) {
        files.push(normalizeSlashes(relativePath));
      }
    }
  }

  return files.sort();
}

function matchesPrefixes(relativePath, prefixes) {
  return prefixes.some((prefix) => relativePath.startsWith(prefix));
}

function requiresRendering(relativePath, prefixes) {
  return prefixes.some((prefix) => relativePath.startsWith(prefix));
}

function generateManifest(options) {
  const root = path.resolve(options.root);
  if (!fs.existsSync(root) || !fs.statSync(root).isDirectory()) {
    throw new Error(`WPT root does not exist or is not a directory: ${root}`);
  }

  const skipped = {};
  const tests = [];

  for (const relativePath of walkFiles(root)) {
    if (!matchesPrefixes(relativePath, options.prefixes)) continue;

    let reason = pathSkipReason(relativePath);
    if (!reason && !options.includeUnsupported) {
      const source = fs.readFileSync(path.join(root, relativePath), 'utf8');
      reason = missingLocalResourceReason(root, relativePath, source) || sourceSkipReason(source);
    }
    if (reason) {
      skipped[reason] = (skipped[reason] || 0) + 1;
      continue;
    }

    const test = {
      name: manifestName(relativePath),
      path: pathToManifestPath(relativePath),
      expected: {
        harness: 'OK',
        subtests: 'all-pass'
      }
    };
    if (requiresRendering(relativePath, options.requiresRenderingPrefixes)) {
      test.requiresRendering = true;
    }
    tests.push(test);
  }

  const limitedTests = options.limit === null ? tests : tests.slice(0, options.limit);
  return {
    generatedBy: 'tools/generate_wpt_manifest.js',
    prefixes: options.prefixes,
    tests: limitedTests,
    skipped
  };
}

function writeManifest(output, manifest) {
  const text = `${JSON.stringify(manifest, null, 2)}\n`;
  if (output === '-') {
    process.stdout.write(text);
  } else {
    fs.writeFileSync(output, text);
  }
}

function main() {
  const args = parseArgs(process.argv);
  const manifest = generateManifest(args);
  writeManifest(args.output, manifest);
  const skippedCount = Object.values(manifest.skipped).reduce((sum, count) => sum + count, 0);
  process.stderr.write(`Generated ${manifest.tests.length} WPT manifest entries (${skippedCount} skipped)\n`);
}

if (require.main === module) {
  try {
    main();
  } catch (error) {
    console.error(`generate_wpt_manifest.js: ${error.message}`);
    process.exit(1);
  }
}

module.exports = {
  generateManifest,
  manifestName,
  normalizePrefix,
  parseArgs,
  pathSkipReason,
  sourceSkipReason
};
