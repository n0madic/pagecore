(function(global) {
  'use strict';

  const PASS = 0;
  const FAIL = 1;
  const HARNESS_OK = 0;
  const tests = [];
  const callbacks = [];
  let pending = 0;
  let completed = false;
  let completionQueued = false;

  function format(value) {
    if (value && value.stack) return String(value.stack);
    try {
      return JSON.stringify(value);
    } catch (_error) {
      return String(value);
    }
  }

  function AssertionError(message) {
    this.name = 'AssertionError';
    this.message = String(message || 'assertion failed');
    if (Error.captureStackTrace) Error.captureStackTrace(this, AssertionError);
  }
  AssertionError.prototype = Object.create(Error.prototype);
  AssertionError.prototype.constructor = AssertionError;

  function record(name, status, message) {
    tests.push({
      name: String(name || ''),
      status,
      message: message == null ? null : String(message)
    });
  }

  function complete() {
    if (completed || pending !== 0) return;
    completed = true;
    const status = { status: HARNESS_OK, message: null };
    for (const callback of callbacks.slice()) callback(tests.slice(), status);
  }

  function queueCompletion() {
    if (completionQueued || completed) return;
    completionQueued = true;
    setTimeout(() => {
      completionQueued = false;
      complete();
    }, 0);
  }

  function addCompletionCallback(callback) {
    callbacks.push(callback);
  }

  global.add_completion_callback = addCompletionCallback;

  global.setup = function() {};

  global.done = function() {
    queueCompletion();
  };

  global.test = function(callback, name) {
    try {
      callback();
      record(name, PASS, null);
    } catch (error) {
      record(name, FAIL, format(error));
    }
    queueCompletion();
  };

  global.promise_test = function(callback, name) {
    pending++;
    Promise.resolve()
      .then(() => callback())
      .then(() => record(name, PASS, null))
      .catch((error) => record(name, FAIL, format(error)))
      .then(() => {
        pending--;
        queueCompletion();
      });
  };

  global.async_test = function(callback, name) {
    pending++;
    let finished = false;
    const t = {
      step(fn) {
        if (finished) return;
        try {
          fn();
        } catch (error) {
          finished = true;
          record(name, FAIL, format(error));
          pending--;
          queueCompletion();
        }
      },
      done() {
        if (finished) return;
        finished = true;
        record(name, PASS, null);
        pending--;
        queueCompletion();
      },
      step_func(fn) {
        return (...args) => t.step(() => fn(...args));
      },
      step_func_done(fn) {
        return (...args) => {
          t.step(() => fn(...args));
          t.done();
        };
      },
      unreached_func(description) {
        return t.step_func(() => {
          throw new AssertionError(description || 'unreached code reached');
        });
      }
    };
    if (typeof callback === 'function') {
      setTimeout(() => t.step(() => callback(t)), 0);
    }
    return t;
  };

  global.assert_true = function(actual, description) {
    if (actual !== true) throw new AssertionError(description || `expected true, got ${format(actual)}`);
  };

  global.assert_false = function(actual, description) {
    if (actual !== false) throw new AssertionError(description || `expected false, got ${format(actual)}`);
  };

  global.assert_equals = function(actual, expected, description) {
    if (actual !== expected) {
      throw new AssertionError(description || `expected ${format(expected)}, got ${format(actual)}`);
    }
  };

  global.assert_not_equals = function(actual, expected, description) {
    if (actual === expected) {
      throw new AssertionError(description || `expected a value different from ${format(expected)}`);
    }
  };

  global.assert_array_equals = function(actual, expected, description) {
    if (!Array.isArray(actual) || !Array.isArray(expected) || actual.length !== expected.length) {
      throw new AssertionError(description || `expected array ${format(expected)}, got ${format(actual)}`);
    }
    for (let i = 0; i < actual.length; i++) {
      if (actual[i] !== expected[i]) {
        throw new AssertionError(description || `array mismatch at ${i}: expected ${format(expected[i])}, got ${format(actual[i])}`);
      }
    }
  };

  global.assert_unreached = function(description) {
    throw new AssertionError(description || 'unreached code reached');
  };
})(self);
