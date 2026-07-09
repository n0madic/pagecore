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
    name: 'forms',
    deps: ['dom', 'web'],
    install(ctx, api) {
      const {
        HTMLFormElement,
        HTMLInputElement,
        HTMLSelectElement,
        HTMLOptionElement,
        HTMLTextAreaElement,
        HTMLButtonElement
      } = api.dom;
      const { Blob, File } = api.web;

      function formControlName(control) {
        return control && typeof control.getAttribute === 'function'
          ? control.getAttribute('name') || ''
          : '';
      }

      function controlType(control) {
        return String((control && control.type) || (control && control.getAttribute && control.getAttribute('type')) || '').toLowerCase();
      }

      function isSuccessfulControl(control, submitter) {
        if (!formControlName(control)) return false;
        if (control.disabled || (control.hasAttribute && control.hasAttribute('disabled'))) return false;

        if (control instanceof HTMLButtonElement) {
          return control === submitter && (control.type === 'submit' || control.type === '');
        }

        if (control instanceof HTMLInputElement) {
          const type = controlType(control);
          if (['button', 'reset'].includes(type)) return false;
          if (['submit', 'image'].includes(type)) return control === submitter;
          if ((type === 'checkbox' || type === 'radio') && !control.checked) return false;
        }

        return control instanceof HTMLInputElement ||
          control instanceof HTMLSelectElement ||
          control instanceof HTMLTextAreaElement;
      }

      function normalizeValue(value, filename = undefined) {
        if (value instanceof Blob) {
          if (value instanceof File && filename === undefined) return value;
          return new File([value], filename === undefined ? 'blob' : String(filename), { type: value.type });
        }
        return String(value);
      }

      function selectedOptions(select) {
        const options = select.options || [];
        const selected = options.filter((option) => option instanceof HTMLOptionElement && option.selected && !option.disabled);
        if (selected.length > 0) return selected;
        const first = options.find((option) => option instanceof HTMLOptionElement && !option.disabled);
        return first ? [first] : [];
      }

      class FormData {
        constructor(form = undefined, submitter = undefined) {
          this._entries = [];
          if (form === undefined) return;
          if (!(form instanceof HTMLFormElement)) throw new TypeError('FormData constructor expects an HTMLFormElement');
          if (submitter !== undefined && !(submitter instanceof HTMLButtonElement || submitter instanceof HTMLInputElement)) {
            throw new TypeError('FormData submitter must be a submit button');
          }

          const controls = form.querySelectorAll('button,input,select,textarea');
          for (const control of controls) {
            if (!isSuccessfulControl(control, submitter)) continue;
            const name = formControlName(control);
            if (control instanceof HTMLSelectElement) {
              for (const option of selectedOptions(control)) this.append(name, option.value);
            } else {
              this.append(name, control.value || '');
            }
          }
        }

        append(name, value, filename = undefined) {
          this._entries.push([String(name), normalizeValue(value, filename)]);
        }

        delete(name) {
          const key = String(name);
          this._entries = this._entries.filter((entry) => entry[0] !== key);
        }

        get(name) {
          const key = String(name);
          const found = this._entries.find((entry) => entry[0] === key);
          return found ? found[1] : null;
        }

        getAll(name) {
          const key = String(name);
          return this._entries.filter((entry) => entry[0] === key).map((entry) => entry[1]);
        }

        has(name) {
          const key = String(name);
          return this._entries.some((entry) => entry[0] === key);
        }

        set(name, value, filename = undefined) {
          const key = String(name);
          const next = [key, normalizeValue(value, filename)];
          let replaced = false;
          const entries = [];
          for (const entry of this._entries) {
            if (entry[0] === key) {
              if (!replaced) {
                entries.push(next);
                replaced = true;
              }
            } else {
              entries.push(entry);
            }
          }
          if (!replaced) entries.push(next);
          this._entries = entries;
        }

        forEach(callback, thisArg = undefined) {
          for (const [name, value] of this._entries) callback.call(thisArg, value, name, this);
        }

        entries() {
          return this._entries.map((entry) => [entry[0], entry[1]])[Symbol.iterator]();
        }

        keys() {
          return this._entries.map((entry) => entry[0])[Symbol.iterator]();
        }

        values() {
          return this._entries.map((entry) => entry[1])[Symbol.iterator]();
        }

        [Symbol.iterator]() { return this.entries(); }
      }

      // Exposed on ctx so the web module (a dependency of forms, and therefore
      // unable to import FormData directly) can recognise a FormData request body
      // and serialize it as multipart/form-data.
      ctx.FormData = FormData;

      return {
        FormData
      };
    }
  };
});
