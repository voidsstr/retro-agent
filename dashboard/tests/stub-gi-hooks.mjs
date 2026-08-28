/* The resolve half of the GI stub — see stub-gi.mjs.
 *
 * The proxy has to answer ToPrimitive, or a harmless
 * `${GLib.DateTime.new_now_local().format('%H:%M')}` in a renderer throws
 * "Cannot convert object to primitive value" and takes the preview with it.
 * Anything coerced to a string becomes a visible placeholder rather than
 * silently reading as empty.
 */
const STUB = 'data:text/javascript,' + encodeURIComponent(`
    const mk = () => new Proxy(function () {}, {
        get: (_t, key) => {
            if (key === Symbol.toPrimitive || key === 'toString' || key === 'valueOf')
                return () => '<stub>';
            if (key === 'then')
                return undefined;          // never look like a thenable
            return mk();
        },
        apply: () => mk(),
        construct: () => mk(),
    });
    const noop = mk();
    export default noop;
    export const Extension = class {};
    export const layoutManager = noop;
    export const Main = noop;
`);

export async function resolve(specifier, context, next) {
    if (specifier.startsWith('gi://') || specifier.startsWith('resource://'))
        return {url: STUB, shortCircuit: true};
    return next(specifier, context);
}
