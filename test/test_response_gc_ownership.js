// Regression test: a large body must survive a GC that runs while the fetch
// handler is still using the response.
//
// Before this fix, ANY allocation big enough to trigger a young-generation GC
// inside fetch() made the Response weak callback fire mid-handler. It
// force-sent an empty body and freed the object, so the client got an empty
// reply and the process then exited (no work left). Threshold on this machine:
// 1 MB worked, 2 MB returned 0 bytes.
//
// Run: build/bin/zane.exe test/test_response_gc_ownership.js
//   curl -s -o /dev/null -w '%{size_download}\n' localhost:3001/?n=4000000

setTimeout(() => {}, 120000); // keep the loop alive while testing

Zane.serve({
  port: 3001,
  fetch(req, res) {
    const m = /n=(\d+)/.exec(req.url);
    const n = m ? parseInt(m[1], 10) : 1000;

    // Big body: builds several MB of garbage before sending.
    res.setHeader("content-type", "text/plain");
    res.send("z".repeat(n));
  }
});

console.log("Server running at http://localhost:3001/");
