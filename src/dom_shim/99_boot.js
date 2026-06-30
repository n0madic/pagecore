(function(root) {
  'use strict';

  function boot(target = root) {
    if (!target || typeof target.__pagecore_dom_shim_install !== 'function') {
      throw new Error('PageCore DOM shim runtime is not installed');
    }
    return target.__pagecore_dom_shim_install(target);
  }

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = { boot };
    return;
  }

  boot(root);
})(globalThis);
