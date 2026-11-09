// close() test: server must shut down, free the port, and let the process exit.
const server = Zane.serve({
    port: 8083,
    hostname: "127.0.0.1",
    fetch(req, res) {
        res.send("close-test");
    },
});

setTimeout(() => {
    console.log("closing server...");
    server.close();
    console.log("server closed, process should exit now");
}, 2000);