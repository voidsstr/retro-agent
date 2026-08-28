/* Loader hooks that let plain node import extension.js.
 *
 * extension.js imports `gi://Clutter` and friends, which only resolve inside
 * gnome-shell. The panel *renderers* underneath are pure string building and
 * are the part worth exercising, so these hooks satisfy the imports with
 * inert stubs. Nothing here pretends to be GNOME — the harness never touches
 * an actor; it replaces this._panels with capture objects first.
 *
 * Used by preview_panels.mjs and test_panels.mjs:
 *   node --import ./dashboard/tests/stub-gi.mjs dashboard/tests/preview_panels.mjs
 */
import {register} from 'node:module';
import {pathToFileURL} from 'node:url';

const STUB = 'data:text/javascript,' + encodeURIComponent(`
    const noop = new Proxy(function () {}, {
        get: () => noop,
        apply: () => noop,
        construct: () => noop,
    });
    export default noop;
    export const Extension = class {};
    export const layoutManager = noop;
`);

register(pathToFileURL('./dashboard/tests/stub-gi-hooks.mjs'));
export {STUB};
