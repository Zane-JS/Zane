Zane.serve({
  port: 3000,
  fetch(req, res) {
    console.log("Method:", req.method);
    console.log("URL:", req.url);
    res.send("Hello from Zane!");
  }
});

console.log("Server running at http://localhost:3000/");
