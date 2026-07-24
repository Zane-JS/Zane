// Regression test for issue #6: Use-After-Free on the Request wrapper.
//
// ROOT CAUSE (before fix): Request was owned by std::unique_ptr inside the
// trantor message callback. The V8 wrapper stored a raw `this` pointer via
// SetInternalField, but registered NO weak callback / finalizer. So as soon as
// the C++ message-callback lambda returned, the unique_ptr deleted the Request
// — while JS could still be holding the wrapper. Any later access dereferenced
// freed memory.
//
// FIX: ownership of Request is transferred into the V8 wrapper (Request::wrap)
// and freed by a V8 weak callback when the wrapper is GC'd — same pattern as
// StreamInternal / zlib / fs streams elsewhere in the codebase.
//
// This test reproduces the UAF condition and asserts it no longer happens:
//   1. Start a server whose fetch() retains each req wrapper.
//   2. On every Nth request, re-read the method/url of ALL previously retained
//      wrappers. By request N, the C++ message callbacks for requests 1..N-1
//      have ALREADY RETURNED — so their unique_ptrs would have freed the
//      underlying Request objects. Pre-fix: reading req.method on an old
//      wrapper was use-after-free (crash / garbage / ASAN error). Post-fix:
//      the objects survive because JS still reaches them via the array.
//
// We run N=25 then assert the final state. A crash (non-zero exit, no output)
// means the UAF still exists.
//
// Usage: ./zane.exe test/test_server_uaf_promise.js
// Driver (separate shell): for i in $(seq 1 25); do curl -s -o /dev/null http://127.0.0.1:8731/; done

const PORT = 8731;
const TARGET = 25;
const retained = [];
let probeFailures = 0;

const server = Zane.serve({
    port: PORT,
    hostname: "127.0.0.1",
    fetch(req, res) {
        retained.push(req);

        // Re-read every previously retained wrapper. For requests 1..N-1 the
        // C++ message callback has already returned; if ownership weren't moved
        // into the wrapper, these reads would touch freed memory.
        for (let i = 0; i < retained.length; i++) {
            const r = retained[i];
            if (typeof r.method !== "string" || r.method !== "GET" ||
                typeof r.url !== "string" || typeof r.pathname !== "string") {
                probeFailures++;
            }
        }

        res.status = 200;
        res.send(`req-${retained.length}`);

        // Final assertion on the TARGET-th request.
        if (retained.length >= TARGET) {
            if (probeFailures > 0) {
                console.error(`[uaf-test] FAIL: ${probeFailures} use-after-free reads detected across ${retained.length} requests`);
                server.close();
                process.exit(1);
            }
            console.log(`[uaf-test] PASS — ${retained.length} retained req wrappers all safe to access (no UAF)`);
            server.close();
            process.exit(0);
        }
    },
});

console.log(`[uaf-test] server on :${PORT}; send ${TARGET} requests to test`);
