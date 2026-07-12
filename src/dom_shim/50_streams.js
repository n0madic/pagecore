(function(root, factory) {
  const definition = factory();
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = definition;
  } else if (root && typeof root.__pagecore_dom_shim_define === 'function') {
    root.__pagecore_dom_shim_define(definition);
  } else if (root) {
    root.PageCoreDomShimModules = root.PageCoreDomShimModules || {};
    root.PageCoreDomShimModules[definition.name] = definition;
  }
})(globalThis, function() {
  'use strict';

  return {
    name: 'streams',
    deps: ['events', 'web'],
    install(ctx, api) {
      const { DOMException } = api.events;
      const { TextEncoder, TextDecoder } = api.web;

      function notSupported(feature) {
        return new DOMException(`${feature} is not supported by this PageCore stream implementation.`, 'NotSupportedError');
      }

      function resolvePromise(value) {
        try {
          return Promise.resolve(value);
        } catch (error) {
          return Promise.reject(error);
        }
      }

      function closeReadableStream(stream) {
        if (stream._state !== 'readable') return;
        stream._state = 'closed';
        while (stream._readRequests.length) {
          stream._readRequests.shift().resolve({ value: undefined, done: true });
        }
        if (stream._reader) stream._reader._closedResolve(undefined);
      }

      function errorReadableStream(stream, error) {
        if (stream._state !== 'readable') return;
        stream._state = 'errored';
        stream._storedError = error;
        while (stream._readRequests.length) stream._readRequests.shift().reject(error);
        if (stream._reader) stream._reader._closedReject(error);
      }

      class ReadableStreamDefaultController {
        constructor(stream) {
          this._stream = stream;
        }

        get desiredSize() {
          return Math.max(0, 1 - this._stream._queue.length);
        }

        enqueue(chunk) {
          const stream = this._stream;
          if (stream._state !== 'readable') throw new TypeError('ReadableStream is not readable');
          if (stream._readRequests.length) {
            stream._readRequests.shift().resolve({ value: chunk, done: false });
          } else {
            stream._queue.push(chunk);
          }
        }

        close() {
          closeReadableStream(this._stream);
        }

        error(error) {
          errorReadableStream(this._stream, error);
        }
      }

      class ReadableStream {
        constructor(underlyingSource = {}, _strategy = {}) {
          this._queue = [];
          this._readRequests = [];
          this._state = 'readable';
          this._storedError = undefined;
          this._reader = null;
          this._pulling = false;
          this._pullAlgorithm = typeof underlyingSource.pull === 'function'
            ? () => underlyingSource.pull(this._controller)
            : null;
          this._cancelAlgorithm = typeof underlyingSource.cancel === 'function'
            ? (reason) => underlyingSource.cancel(reason)
            : null;
          this._controller = new ReadableStreamDefaultController(this);

          if (underlyingSource && underlyingSource.type === 'bytes') {
            throw notSupported('Readable byte streams');
          }

          if (underlyingSource && typeof underlyingSource.start === 'function') {
            try {
              resolvePromise(underlyingSource.start(this._controller)).catch((error) => {
                errorReadableStream(this, error);
              });
            } catch (error) {
              errorReadableStream(this, error);
            }
          }
        }

        get locked() {
          return this._reader !== null;
        }

        getReader(options = undefined) {
          if (options && options.mode === 'byob') throw notSupported('ReadableStream BYOB readers');
          return new ReadableStreamDefaultReader(this);
        }

        cancel(reason = undefined) {
          if (this.locked) return Promise.reject(new TypeError('Cannot cancel a locked ReadableStream'));
          this._queue.length = 0;
          closeReadableStream(this);
          return this._cancelAlgorithm ? resolvePromise(this._cancelAlgorithm(reason)) : Promise.resolve();
        }

        tee() {
          const reader = this.getReader();
          const source = this;
          const branches = [null, null];
          const cancelled = [false, false];
          const reasons = [undefined, undefined];

          const cancelSource = (reason) => {
            source._queue.length = 0;
            closeReadableStream(source);
            if (source._cancelAlgorithm) {
              try { resolvePromise(source._cancelAlgorithm(reason)); } catch (_cancelError) {}
            }
          };

          const makeUnderlyingSource = (index) => ({
            cancel(reason) {
              cancelled[index] = true;
              reasons[index] = reason;
              if (cancelled[0] && cancelled[1]) {
                cancelSource(reasons[0]);
              }
              return Promise.resolve();
            }
          });

          branches[0] = new ReadableStream(makeUnderlyingSource(0));
          branches[1] = new ReadableStream(makeUnderlyingSource(1));

          // Only act on a branch whose controller is still readable; a cancelled
          // branch's controller would throw on enqueue/close.
          const toReadable = (branch, fn) => {
            if (branch._state === 'readable') {
              try { fn(branch._controller); } catch (_branchError) {}
            }
          };

          const pump = () => reader.read().then((result) => {
            if (result.done) {
              toReadable(branches[0], (c) => c.close());
              toReadable(branches[1], (c) => c.close());
              return undefined;
            }
            toReadable(branches[0], (c) => c.enqueue(result.value));
            toReadable(branches[1], (c) => c.enqueue(result.value));
            if (cancelled[0] && cancelled[1]) return undefined;
            return pump();
          }, (error) => {
            toReadable(branches[0], (c) => c.error(error));
            toReadable(branches[1], (c) => c.error(error));
          });
          // Avoid an unhandled rejection if the pump chain rejects.
          pump().catch(() => {});
          return branches;
        }

        pipeTo(destination, _options = {}) {
          const reader = this.getReader();
          const writer = destination && typeof destination.getWriter === 'function'
            ? destination.getWriter()
            : null;
          if (!writer) return Promise.reject(new TypeError('pipeTo destination must be a WritableStream'));

          const pump = () => reader.read().then((result) => {
            if (result.done) return writer.close();
            return writer.write(result.value).then(pump);
          });

          return pump().finally(() => {
            try { reader.releaseLock(); } catch (_error) {}
            try { writer.releaseLock(); } catch (_error) {}
          });
        }

        pipeThrough(transform, options = {}) {
          this.pipeTo(transform.writable, options);
          return transform.readable;
        }

        [Symbol.asyncIterator]() {
          const reader = this.getReader();
          return {
            next() {
              return reader.read();
            },
            return() {
              reader.releaseLock();
              return Promise.resolve({ value: undefined, done: true });
            }
          };
        }

        static _fromBody(body = '') {
          const bytes = body instanceof Uint8Array ? body : new TextEncoder().encode(String(body ?? ''));
          return new ReadableStream({
            start(controller) {
              controller.enqueue(bytes);
              controller.close();
            }
          });
        }
      }

      class ReadableStreamDefaultReader {
        constructor(stream) {
          if (!(stream instanceof ReadableStream)) throw new TypeError('ReadableStreamDefaultReader expects a ReadableStream');
          if (stream.locked) throw new TypeError('ReadableStream is already locked');
          this._stream = stream;
          stream._reader = this;
          this.closed = new Promise((resolve, reject) => {
            this._closedResolve = resolve;
            this._closedReject = reject;
          });
          if (stream._state === 'closed') this._closedResolve(undefined);
          if (stream._state === 'errored') this._closedReject(stream._storedError);
        }

        read() {
          const stream = this._stream;
          if (!stream) return Promise.reject(new TypeError('Reader has no stream'));
          if (stream._queue.length) return Promise.resolve({ value: stream._queue.shift(), done: false });
          if (stream._state === 'closed') return Promise.resolve({ value: undefined, done: true });
          if (stream._state === 'errored') return Promise.reject(stream._storedError);

          if (stream._pullAlgorithm && !stream._pulling) {
            stream._pulling = true;
            resolvePromise(stream._pullAlgorithm()).catch((error) => {
              errorReadableStream(stream, error);
            }).finally(() => {
              stream._pulling = false;
            });
          }

          if (stream._queue.length) return Promise.resolve({ value: stream._queue.shift(), done: false });
          if (stream._state === 'closed') return Promise.resolve({ value: undefined, done: true });
          if (stream._state === 'errored') return Promise.reject(stream._storedError);

          return new Promise((resolve, reject) => {
            stream._readRequests.push({ resolve, reject });
          });
        }

        cancel(reason = undefined) {
          return this._stream ? this._stream.cancel(reason) : Promise.resolve();
        }

        releaseLock() {
          if (!this._stream) return;
          if (this._stream._readRequests.length) throw new TypeError('Cannot release a reader with pending read requests');
          this._stream._reader = null;
          this._stream = null;
        }
      }

      class WritableStream {
        constructor(underlyingSink = {}, _strategy = {}) {
          this._sink = underlyingSink || {};
          this._state = 'writable';
          this._writer = null;
          this._storedError = undefined;
          // Provide a controller so sinks that call controller.error(...) work.
          this._controller = {
            error: (reason) => this._error(reason)
          };
          if (typeof this._sink.start === 'function') {
            resolvePromise(this._sink.start(this._controller)).catch((reason) => this._error(reason));
          }
        }

        get locked() {
          return this._writer !== null;
        }

        getWriter() {
          return new WritableStreamDefaultWriter(this);
        }

        // Mirrors errorReadableStream(): transitions state and rejects the
        // attached writer's `closed`/`ready` promises, so a write() rejection
        // (e.g. a TransformStream sink whose transform() threw) is observable
        // there too, not just as the write() call's own rejection.
        _error(reason) {
          if (this._state !== 'writable') return;
          this._state = 'errored';
          this._storedError = reason;
          if (this._writer) this._writer._errored(reason);
        }

        _write(chunk) {
          if (this._state !== 'writable') {
            return Promise.reject(this._state === 'errored' ? this._storedError : new TypeError('WritableStream is not writable'));
          }
          const result = typeof this._sink.write === 'function'
            ? resolvePromise(this._sink.write(chunk))
            : Promise.resolve();
          return result.catch((reason) => {
            this._error(reason);
            throw reason;
          });
        }

        _close() {
          if (this._state === 'closed') return Promise.resolve();
          this._state = 'closed';
          const result = typeof this._sink.close === 'function'
            ? resolvePromise(this._sink.close())
            : Promise.resolve();
          if (this._writer) result.then(() => this._writer._closedSuccessfully(), (reason) => this._writer._errored(reason));
          return result;
        }

        _abort(reason) {
          this._state = 'errored';
          this._storedError = reason;
          if (this._writer) this._writer._errored(reason);
          return typeof this._sink.abort === 'function'
            ? resolvePromise(this._sink.abort(reason))
            : Promise.resolve();
        }

        close() { return this._close(); }
        abort(reason = undefined) { return this._abort(reason); }
      }

      class WritableStreamDefaultWriter {
        constructor(stream) {
          if (!(stream instanceof WritableStream)) throw new TypeError('WritableStreamDefaultWriter expects a WritableStream');
          if (stream.locked) throw new TypeError('WritableStream is already locked');
          this._stream = stream;
          stream._writer = this;
          this.ready = Promise.resolve();
          this.desiredSize = 1;
          if (stream._state === 'errored') {
            this.closed = Promise.reject(stream._storedError);
          } else if (stream._state === 'closed') {
            this.closed = Promise.resolve();
          } else {
            this.closed = new Promise((resolve, reject) => {
              this._closedResolve = resolve;
              this._closedReject = reject;
            });
          }
        }

        // Mirrors ReadableStreamDefaultReader's _closedResolve/_closedReject:
        // called by the stream when it transitions after this writer already
        // has a pending `closed` promise.
        _closedSuccessfully() {
          if (this._closedResolve) this._closedResolve(undefined);
        }
        _errored(reason) {
          if (this._closedReject) this._closedReject(reason);
        }

        write(chunk) {
          return this._stream ? this._stream._write(chunk) : Promise.reject(new TypeError('Writer has no stream'));
        }

        close() {
          return this._stream ? this._stream._close() : Promise.resolve();
        }

        abort(reason = undefined) {
          return this._stream ? this._stream._abort(reason) : Promise.resolve();
        }

        releaseLock() {
          if (!this._stream) return;
          this._stream._writer = null;
          this._stream = null;
        }
      }

      class TransformStream {
        constructor(transformer = {}, _writableStrategy = {}, _readableStrategy = {}) {
          let readableController = null;
          // A transform()/flush() that throws (e.g. TextDecoderStream hitting
          // a fatal decode error) must error *both* sides: the readable side
          // via this controller, and the write()/close() promise itself. A
          // synchronous throw happens before resolvePromise() ever runs, so
          // it needs its own catch, not just a .catch() on the settled value.
          const runStep = (fn) => {
            try {
              return resolvePromise(fn()).catch((error) => {
                readableController.error(error);
                throw error;
              });
            } catch (error) {
              readableController.error(error);
              return Promise.reject(error);
            }
          };
          this.readable = new ReadableStream({
            start(controller) {
              readableController = controller;
            }
          });
          this.writable = new WritableStream({
            write(chunk) {
              if (transformer && typeof transformer.transform === 'function') {
                return runStep(() => transformer.transform(chunk, {
                  enqueue(value) { readableController.enqueue(value); },
                  error(error) { readableController.error(error); },
                  terminate() { readableController.close(); }
                }));
              }
              readableController.enqueue(chunk);
              return Promise.resolve();
            },
            close() {
              if (transformer && typeof transformer.flush === 'function') {
                return runStep(() => transformer.flush({
                  enqueue(value) { readableController.enqueue(value); },
                  error(error) { readableController.error(error); },
                  terminate() { readableController.close(); }
                })).then(() => readableController.close());
              }
              readableController.close();
              return Promise.resolve();
            }
          });
        }
      }

      class ByteLengthQueuingStrategy {
        constructor(init) {
          this.highWaterMark = Number(init && init.highWaterMark);
        }
        size(chunk) {
          return chunk && chunk.byteLength ? chunk.byteLength : 0;
        }
      }

      class CountQueuingStrategy {
        constructor(init) {
          this.highWaterMark = Number(init && init.highWaterMark);
        }
        size() { return 1; }
      }

      class ReadableByteStreamController {
        constructor() { throw notSupported('ReadableByteStreamController'); }
      }
      class ReadableStreamBYOBReader {
        constructor() { throw notSupported('ReadableStreamBYOBReader'); }
      }
      class ReadableStreamBYOBRequest {
        constructor() { throw notSupported('ReadableStreamBYOBRequest'); }
      }
      class WritableStreamDefaultController {}
      class TransformStreamDefaultController {}

      // Generic transform streams over TextDecoder/TextEncoder: each chunk is
      // decoded/encoded with `{stream: true}` so a multi-byte sequence split
      // across chunks still resolves correctly, and flush() finalizes the
      // sequence when the writable side closes.
      function isBufferSource(value) {
        return value instanceof ArrayBuffer || ArrayBuffer.isView(value);
      }

      class TextDecoderStream {
        constructor(label = 'utf-8', options = {}) {
          const decoder = new TextDecoder(label, options);
          this._decoder = decoder;
          this._transform = new TransformStream({
            transform(chunk, controller) {
              // Per spec each written chunk must itself be a BufferSource;
              // anything else errors both the readable and writable sides
              // (see TransformStream's runStep, which catches this throw).
              if (!isBufferSource(chunk)) {
                throw new TypeError('TextDecoderStream chunk must be a BufferSource');
              }
              const text = decoder.decode(chunk, { stream: true });
              if (text) controller.enqueue(text);
            },
            flush(controller) {
              const text = decoder.decode();
              if (text) controller.enqueue(text);
            }
          });
        }
        get encoding() { return this._decoder.encoding; }
        get fatal() { return this._decoder.fatal; }
        get ignoreBOM() { return this._decoder.ignoreBOM; }
        get readable() { return this._transform.readable; }
        get writable() { return this._transform.writable; }
      }

      class TextEncoderStream {
        constructor() {
          const encoder = new TextEncoder();
          this._encoder = encoder;
          // A lone leading (high) surrogate at the very end of a chunk might
          // be completed by a trailing surrogate at the start of the next
          // chunk, so it is held back rather than replaced immediately; a
          // lone trailing surrogate, or a leading surrogate followed by
          // anything else, is not ambiguous and is replaced right away by
          // encoder.encode()'s own USVString conversion.
          let pendingHighSurrogate = null;
          this._transform = new TransformStream({
            transform(chunk, controller) {
              let text = String(chunk);
              if (pendingHighSurrogate !== null) {
                text = pendingHighSurrogate + text;
                pendingHighSurrogate = null;
              }
              const lastCode = text.length ? text.charCodeAt(text.length - 1) : 0;
              if (lastCode >= 0xd800 && lastCode <= 0xdbff) {
                pendingHighSurrogate = text[text.length - 1];
                text = text.slice(0, -1);
              }
              if (text.length) controller.enqueue(encoder.encode(text));
            },
            flush(controller) {
              if (pendingHighSurrogate !== null) {
                controller.enqueue(encoder.encode(pendingHighSurrogate));
                pendingHighSurrogate = null;
              }
            }
          });
        }
        get encoding() { return this._encoder.encoding; }
        get readable() { return this._transform.readable; }
        get writable() { return this._transform.writable; }
      }

      return {
        ReadableStream,
        ReadableStreamDefaultController,
        ReadableStreamDefaultReader,
        ReadableByteStreamController,
        ReadableStreamBYOBReader,
        ReadableStreamBYOBRequest,
        WritableStream,
        WritableStreamDefaultWriter,
        WritableStreamDefaultController,
        TransformStream,
        TransformStreamDefaultController,
        ByteLengthQueuingStrategy,
        CountQueuingStrategy,
        TextDecoderStream,
        TextEncoderStream
      };
    }
  };
});
