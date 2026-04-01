// Debug HTTP Server Test
import http from 'node:http';

console.log('[1] Starting...');

const timer = setInterval(() => {
    console.log('[TIMER] Heartbeat');
}, 1000);

console.log('[2] Timer created');

const server = http.createServer((req, res) => {
    console.log(`[REQUEST] ${req.method} ${req.url}`);
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('Hello!\n');
});

console.log('[3] Server created');

server.listen(3000, '127.0.0.1', () => {
    console.log('[4] Server listening');
});

console.log('[5] Setup complete - entering event loop');
