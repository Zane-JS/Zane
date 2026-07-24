// Regression test for issue #7: HTTP Response Splitting / Header Injection.
//
// ROOT CAUSE (before fix): Response::setHeader stored name/value verbatim and
// buildResponse() concatenated them raw into the wire response. App-supplied
// data containing CR/LF could inject arbitrary headers or smuggle a second
// response ("HTTP response splitting").
//
// FIX (two layers, defense-in-depth):
//   1. Response::setHeader / res.setHeader(name,value) reject CR/LF/NUL (and
//      empty name) with a RangeError at the application-facing API.
//   2. buildResponse() (the serializer in libs/http) also strips CR/LF/NUL,
//      so even an internal/third-party caller that bypassed (1) cannot corrupt
//      the wire format.
//
// This test exercises layer 1 from JS: it must throw on injection attempts and
// must still accept well-formed headers.
//
// Usage: ./zane.exe test/test_response_splitting.js

let pass = 0;
let fail = 0;

function expectThrow(label, fn) {
    try {
        fn();
        console.error(`[splitting] FAIL: ${label} — expected RangeError, none thrown`);
        fail++;
    } catch (e) {
        pass++;
        console.log(`[splitting] OK   ${label} → threw ${e.name}`);
    }
}

function expectOk(label, fn) {
    try {
        fn();
        pass++;
        console.log(`[splitting] OK   ${label}`);
    } catch (e) {
        console.error(`[splitting] FAIL: ${label} — unexpected ${e.name}: ${e.message}`);
        fail++;
    }
}

// Build a throwaway Response wrapper. We do this by spinning up a server and
// capturing the `res` from a self-request — but Zane has no HTTP client yet, so
// instead we drive one request over the wire and inspect behaviour inside
// fetch(). The driver script fires a single request.
//
// All assertions run inside the fetch handler where `res` is available.

const PORT = 8741;
let assertionsDone = false;

const server = Zane.serve({
    port: PORT,
    hostname: "127.0.0.1",
    fetch(req, res) {
        if (assertionsDone) { res.send("done"); return; }
        assertionsDone = true;

        // --- Should THROW (response splitting / injection attempts) ---
        expectThrow("CRLF in value",        () => res.setHeader("X-A", "evil\r\nSet-Cookie: admin=1"));
        expectThrow("LF in value",          () => res.setHeader("X-B", "evil\nSet-Cookie: admin=1"));
        expectThrow("CR in value",          () => res.setHeader("X-C", "evil\rSet-Cookie: admin=1"));
        expectThrow("NUL in value",         () => res.setHeader("X-D", "evil\0x"));
        expectThrow("CRLF in name",         () => res.setHeader("X-E\r\nInjected: 1", "v"));
        expectThrow("LF in name",           () => res.setHeader("X-F\nInjected: 1", "v"));
        expectThrow("empty name",           () => res.setHeader("", "v"));

        // --- Should SUCCEED (well-formed headers) ---
        expectOk("normal header",          () => res.setHeader("X-Ok", "value"));
        expectOk("header with space value", () => res.setHeader("X-Space", "a b c"));
        expectOk("header with colon value",() => res.setHeader("X-Colon", "a:b:c")); // colon in VALUE is fine

        // Report
        res.setHeader("X-Report", `pass=${pass} fail=${fail}`);
        res.status = fail === 0 ? 200 : 500;
        res.send(`splitting-test pass=${pass} fail=${fail}`);

        if (fail > 0) {
            console.error(`[splitting] FAIL: ${fail} assertion(s) failed`);
            server.close();
            process.exit(1);
        }
        console.log(`[splitting] PASS — ${pass} assertions ok, response splitting blocked`);
        server.close();
        process.exit(0);
    },
});

console.log(`[splitting] server on :${PORT}; send 1 request to run assertions`);
