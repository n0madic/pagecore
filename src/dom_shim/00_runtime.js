(function(root, factory) {
  const installRuntime = factory();
  if (typeof module !== 'undefined' && module.exports) {
    module.exports = installRuntime;
  }
  installRuntime(root);
})(globalThis, function() {
  'use strict';

  function installRuntime(root) {
    const definitions = [];

    function define(definition) {
      if (!definition || typeof definition.name !== 'string' || typeof definition.install !== 'function') {
        throw new TypeError('Invalid PageCore DOM shim module definition');
      }
      definitions.push(definition);
    }

    function install(target = root) {
      const byName = new Map();
      for (const definition of definitions) {
        if (byName.has(definition.name)) {
          throw new Error('Duplicate PageCore DOM shim module: ' + definition.name);
        }
        byName.set(definition.name, definition);
      }

      const ctx = {
        global: target,
        bridge: target.__dom,
        host: target.__host
      };
      const exportsByName = Object.create(null);
      const installing = new Set();
      const installed = new Set();

      function installModule(name) {
        if (installed.has(name)) return exportsByName[name];
        const definition = byName.get(name);
        if (!definition) throw new Error('Missing PageCore DOM shim module: ' + name);
        if (installing.has(name)) throw new Error('Circular PageCore DOM shim module dependency: ' + name);

        installing.add(name);
        const api = Object.create(null);
        for (const dependency of definition.deps || []) {
          api[dependency] = installModule(dependency);
        }
        const exports = definition.install(ctx, api) || Object.create(null);
        exportsByName[name] = exports;
        installed.add(name);
        installing.delete(name);
        return exports;
      }

      for (const definition of definitions) installModule(definition.name);
      delete target.__pagecore_dom_shim_define;
      delete target.__pagecore_dom_shim_install;
      return exportsByName;
    }

    Object.defineProperty(root, '__pagecore_dom_shim_define', {
      value: define,
      configurable: true
    });
    Object.defineProperty(root, '__pagecore_dom_shim_install', {
      value: install,
      configurable: true
    });
  }

  return installRuntime;
});
