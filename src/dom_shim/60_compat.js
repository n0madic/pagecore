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
    name: 'compat',
    deps: ['events', 'dom'],
    install(ctx, api) {
      const { global } = ctx;
      const { DOMException, EventTarget } = api.events;
      const { document } = api.dom;

      const nativeBase64Encode = ctx.host && typeof ctx.host.base64Encode === 'function'
        ? ctx.host.base64Encode
        : null;
      const nativeBase64Decode = ctx.host && typeof ctx.host.base64Decode === 'function'
        ? ctx.host.base64Decode
        : null;
      if (!nativeBase64Encode || !nativeBase64Decode) {
        throw new Error('PageCore base64 host bridge is not installed.');
      }

      function invalidCharacter(message) {
        return new DOMException(message, 'InvalidCharacterError');
      }

      function btoa(input = '') {
        const text = String(input);
        for (let index = 0; index < text.length; index++) {
          if (text.charCodeAt(index) > 0xff) {
            throw invalidCharacter('The string to be encoded contains characters outside of the Latin1 range.');
          }
        }
        try {
          return nativeBase64Encode(text);
        } catch (_nativeError) {
          throw invalidCharacter('The string to be encoded contains characters outside of the Latin1 range.');
        }
      }

      function normalizeBase64Input(input = '') {
        let text = String(input).replace(/[\t\n\f\r ]+/g, '');
        if (text.length % 4 === 0) {
          const paddingMatch = /=+$/.exec(text);
          if (paddingMatch) {
            if (paddingMatch[0].length > 2) {
              throw invalidCharacter('The string to be decoded is not correctly encoded.');
            }
            text = text.slice(0, -paddingMatch[0].length);
          }
        } else if (text.indexOf('=') !== -1) {
          throw invalidCharacter('The string to be decoded is not correctly encoded.');
        }
        if (text.length % 4 === 1 || /[^A-Za-z0-9+/]/.test(text)) {
          throw invalidCharacter('The string to be decoded is not correctly encoded.');
        }
        return text;
      }

      function atob(input = '') {
        const text = normalizeBase64Input(input);
        try {
          return nativeBase64Decode(text);
        } catch (_nativeError) {
          throw invalidCharacter('The string to be decoded is not correctly encoded.');
        }
      }

      function cssEscape(value = '') {
        const string = String(value);
        let result = '';
        for (let index = 0; index < string.length; index++) {
          const code = string.charCodeAt(index);
          const char = string.charAt(index);
          if (code === 0x0000) {
            result += '\uFFFD';
          } else if (
            (code >= 0x0001 && code <= 0x001f) ||
            code === 0x007f ||
            (index === 0 && code >= 0x0030 && code <= 0x0039) ||
            (index === 1 && code >= 0x0030 && code <= 0x0039 && string.charAt(0) === '-')
          ) {
            result += `\\${code.toString(16)} `;
          } else if (
            code >= 0x0080 ||
            char === '-' ||
            char === '_' ||
            (code >= 0x0030 && code <= 0x0039) ||
            (code >= 0x0041 && code <= 0x005a) ||
            (code >= 0x0061 && code <= 0x007a)
          ) {
            result += char;
          } else {
            result += `\\${char}`;
          }
        }
        return result;
      }

      function supportsDeclaration(property, value) {
        const name = String(property || '').trim();
        const text = String(value || '').trim();
        if (!name || !text) return false;
        if (!/^--[A-Za-z0-9_-]+$/.test(name) && !/^-?[A-Za-z_][A-Za-z0-9_-]*$/.test(name)) return false;
        if (/[{};]/.test(text)) return false;
        return true;
      }

      function cssSupports(propertyOrCondition, value = undefined) {
        if (value !== undefined) return supportsDeclaration(propertyOrCondition, value);
        const condition = String(propertyOrCondition || '').trim();
        const match = /^\(\s*([^:()]+)\s*:\s*([^()]+)\s*\)$/.exec(condition);
        if (match) return supportsDeclaration(match[1], match[2]);
        if (/^(not|and|or)\b/i.test(condition)) return false;
        return false;
      }

      const CSS = {
        escape: cssEscape,
        supports: cssSupports
      };

      function Audio(src = undefined) {
        const audio = document.createElement('audio');
        if (src !== undefined) audio.src = String(src);
        return audio;
      }

      function Option(text = '', value = undefined, defaultSelected = false, selected = false) {
        const option = document.createElement('option');
        option.text = String(text);
        if (value !== undefined) option.value = String(value);
        option.defaultSelected = Boolean(defaultSelected);
        option.selected = Boolean(selected);
        return option;
      }

      function unsupportedWorkerError(kind) {
        return new DOMException(`${kind} are not supported by this PageCore runtime.`, 'NotSupportedError');
      }

      class Worker extends EventTarget {
        constructor() {
          super();
          throw unsupportedWorkerError('Dedicated workers');
        }
      }

      class SharedWorker extends EventTarget {
        constructor() {
          super();
          throw unsupportedWorkerError('Shared workers');
        }
      }

      class ServiceWorkerContainer extends EventTarget {
        constructor() {
          super();
          this.controller = null;
        }

        get ready() {
          return Promise.reject(unsupportedWorkerError('Service workers'));
        }

        register() {
          return Promise.reject(unsupportedWorkerError('Service workers'));
        }

        getRegistration() {
          return Promise.resolve(undefined);
        }

        getRegistrations() {
          return Promise.resolve([]);
        }
      }

      function createIntlFallback(existing = undefined) {
        const intl = existing || {};

        if (!intl.DateTimeFormat) {
          intl.DateTimeFormat = function DateTimeFormat(locales, options) {
            this.locales = locales;
            this.options = options || {};
          };
          intl.DateTimeFormat.prototype.format = function(date = new Date()) {
            const value = date instanceof Date ? date : new Date(date);
            return Number.isNaN(value.getTime()) ? 'Invalid Date' : value.toISOString();
          };
          intl.DateTimeFormat.prototype.formatToParts = function(date = new Date()) {
            return [{ type: 'literal', value: this.format(date) }];
          };
          intl.DateTimeFormat.prototype.resolvedOptions = function() {
            return { locale: 'en-US', calendar: 'gregory', numberingSystem: 'latn', timeZone: 'UTC' };
          };
          intl.DateTimeFormat.supportedLocalesOf = (locales) => Array.isArray(locales) ? locales : (locales ? [locales] : []);
        }

        if (!intl.NumberFormat) {
          intl.NumberFormat = function NumberFormat(locales, options) {
            this.locales = locales;
            this.options = options || {};
          };
          intl.NumberFormat.prototype.format = (number) => String(number);
          intl.NumberFormat.prototype.formatToParts = (number) => [{ type: 'integer', value: String(number) }];
          intl.NumberFormat.prototype.resolvedOptions = () => ({ locale: 'en-US', numberingSystem: 'latn', style: 'decimal' });
          intl.NumberFormat.supportedLocalesOf = (locales) => Array.isArray(locales) ? locales : (locales ? [locales] : []);
        }

        if (!intl.Collator) {
          intl.Collator = function Collator() {};
          intl.Collator.prototype.compare = (left, right) => String(left).localeCompare(String(right));
          intl.Collator.prototype.resolvedOptions = () => ({ locale: 'en-US', usage: 'sort', sensitivity: 'variant' });
          intl.Collator.supportedLocalesOf = (locales) => Array.isArray(locales) ? locales : (locales ? [locales] : []);
        }

        if (!intl.PluralRules) {
          intl.PluralRules = function PluralRules() {};
          intl.PluralRules.prototype.select = (number) => Number(number) === 1 ? 'one' : 'other';
          intl.PluralRules.prototype.resolvedOptions = () => ({ locale: 'en-US', type: 'cardinal' });
          intl.PluralRules.supportedLocalesOf = (locales) => Array.isArray(locales) ? locales : (locales ? [locales] : []);
        }

        if (!intl.RelativeTimeFormat) {
          intl.RelativeTimeFormat = function RelativeTimeFormat() {};
          intl.RelativeTimeFormat.prototype.format = function(value, unit) {
            const absolute = Math.abs(Number(value));
            const suffix = absolute === 1 ? String(unit) : `${unit}s`;
            return Number(value) < 0 ? `${absolute} ${suffix} ago` : `in ${absolute} ${suffix}`;
          };
          intl.RelativeTimeFormat.prototype.formatToParts = function(value, unit) {
            return [{ type: 'literal', value: this.format(value, unit) }];
          };
          intl.RelativeTimeFormat.prototype.resolvedOptions = () => ({ locale: 'en-US', style: 'long', numeric: 'always' });
          intl.RelativeTimeFormat.supportedLocalesOf = (locales) => Array.isArray(locales) ? locales : (locales ? [locales] : []);
        }

        if (!intl.ListFormat) {
          intl.ListFormat = function ListFormat(_locales, options = {}) {
            this.type = options.type || 'conjunction';
          };
          intl.ListFormat.prototype.format = function(list) {
            const values = Array.from(list || [], (value) => String(value));
            if (values.length <= 1) return values[0] || '';
            const last = values.pop();
            return `${values.join(', ')}${this.type === 'disjunction' ? ' or ' : ' and '}${last}`;
          };
          intl.ListFormat.prototype.formatToParts = function(list) {
            return [{ type: 'literal', value: this.format(list) }];
          };
          intl.ListFormat.prototype.resolvedOptions = function() {
            return { locale: 'en-US', type: this.type, style: 'long' };
          };
          intl.ListFormat.supportedLocalesOf = (locales) => Array.isArray(locales) ? locales : (locales ? [locales] : []);
        }

        if (!intl.Segmenter) {
          intl.Segmenter = function Segmenter(_locales, options = {}) {
            this.granularity = options.granularity || 'grapheme';
          };
          intl.Segmenter.prototype.segment = function(input) {
            const source = String(input);
            const segments = Array.from(source, (segment, index) => ({ segment, index, input: source }));
            segments.containing = (index) => segments.find((entry) => entry.index <= index && index < entry.index + entry.segment.length);
            return segments;
          };
          intl.Segmenter.prototype.resolvedOptions = function() {
            return { locale: 'en-US', granularity: this.granularity };
          };
          intl.Segmenter.supportedLocalesOf = (locales) => Array.isArray(locales) ? locales : (locales ? [locales] : []);
        }

        return intl;
      }

      void global;
      return {
        CSS,
        btoa,
        atob,
        Audio,
        Option,
        Worker,
        SharedWorker,
        ServiceWorkerContainer,
        createIntlFallback
      };
    }
  };
});
