// Regression test for the V8 main-thread safety bug in Zane.serve().
//
// Before the fix, the Trantor loop thread called the JS fetch handler directly
// (with its own v8::Locker). Anything the handler did that touches the isolate's
// per-thread state — setTimeout, fs, queueMicrotask, JSON on a growing heap —
// raced with the main loop and produced crashes or hangs.
//
// Run: build/bin/zane.exe test/test_server_main_thread_safety.js
// Then: curl repeatedly, e.g.
//   for i in $(seq 1 200); do curl -s localhost:3000/async >/dev/null & done; wait

let async_count = 0;

Zane.serve({
  port: 3000,
  fetch(req, res) {
    if (req.url.startsWith("/async")) {
      // setTimeout schedules on the main thread's timer queue: this is the
      // exact pattern that used to corrupt state when fetch ran off-thread.
      setTimeout(() => {
        res.send("async-ok-" + (++async_count));
      }, 1);
      return;
    }

    if (req.url.startsWith("/microtask")) {
      // Microtask checkpoint from a handler invoked off-thread was the other
      // classic crash: the checkpoint races with the main loop's own.
      Promise.resolve().then(() => {
        res.send("microtask-ok");
      });
      return;
    }

    if (req.url.startsWith("/heavy")) {
      // Allocate enough to trigger a GC while the request is in flight.
      const arr = [];
      for (let i = 0; i < 50000; i++) arr.push({ i, s: "x" + i });
      res.send("heavy-ok-" + arr.length);
      return;
    }

    res.send("Hello from Zane!");
  }
});

console.log("Server running at http://localhost:3000/");
