#!/usr/bin/env node
'use strict';

const assert = require('assert');
const path = require('path');

const { isUnderRoot } = require(path.resolve(__dirname, '..', '..', 'tools', 'run_display_list_reftests.js'));

function test(name, fn) {
  try {
    fn();
    console.log(`ok - ${name}`);
  } catch (error) {
    console.error(`not ok - ${name}`);
    throw error;
  }
}

test('isUnderRoot treats a plain in-root file as contained', () => {
  assert.ok(isUnderRoot('/a/b', path.join('/a/b', 'foo.json')));
});

test('isUnderRoot treats the root itself as contained', () => {
  assert.ok(isUnderRoot('/a/b', '/a/b'));
});

test('isUnderRoot does not mistake a name starting with ".." for an escape', () => {
  // Regression test: a directory or file literally named starting with ".."
  // (e.g. "..cache") is a normal in-root descendant, not a traversal escape.
  assert.ok(isUnderRoot('/a/b', path.join('/a/b', '..cache', 'foo.json')));
});

test('isUnderRoot rejects a sibling directory', () => {
  assert.ok(!isUnderRoot('/a/b', path.join('/a/other', 'foo.json')));
});

test('isUnderRoot rejects the parent directory itself', () => {
  assert.ok(!isUnderRoot('/a/b', '/a'));
});

console.log('all run_display_list_reftests.js unit tests passed');
