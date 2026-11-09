// Smoke test: Zane.serve() on the Drogon backend.
// Usage: .\zane.exe test\test_serve_drogon.js
// Driver: curl -s http://127.0.0.1:8082/ ...
const server = Zane.serve({
    port: 8082,
    hostname: "127.0.0.1",
    fetch(req, res) {
        console.log(`[fetch] ${req.method} ${req.url} pathname=${req.pathname} hdr_ua=${req.headers["user-agent"] || ""}`);

        if (req.pathname === "/json") {
            res.status = 201;
            res.json({ hello: "world", n: 42 });
            return;
        }
        if (req.pathname === "/stream") {
            res.writeHead(200, { "Content-Type": "text/plain" });
            res.write("chunk1-");
            res.write("chunk2");
            res.end();
            return;
        }
        if (req.pathname === "/async") {
            setTimeout(() => {
                res.status = 202;
                res.send("delayed response");
            }, 300);
            return;
        }
        if (req.pathname === "/promise") {
            return Promise.resolve().then(() => {
                res.send("promise response");
            });
        }
        if (req.pathname === "/echo") {
            res.send(`method=${req.method} body=${req.text()}`);
            return;
        }
        if (req.pathname === "/status404") {
            res.status = 404;
            res.send("custom 404 text");
            return;
        }
        res.status = 200;
        res.send(`Hello from Zane + Drogon! (${req.method} ${req.url})`);
    },
});

console.log("drogon test server on :8082");