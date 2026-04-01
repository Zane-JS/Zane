// Simple HTTP test
import http from 'node:http';

console.log('Testing HTTP module...');
console.log('METHODS:', http.METHODS);
console.log('STATUS_CODES:', http.STATUS_CODES);

const server = http.createServer((req, res) => {
    console.log('Request received:', req.method, req.url);
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('Hello from Z8!\n');
});

console.log('Server object created');
console.log('Calling listen...');

server.listen(3000, '127.0.0.1', () => {
    console.log('Server is listening on port 3000');
});

console.log('Listen called, keeping alive...');

// Keep alive with setInterval
setInterval(() => {
    console.log('Server still running...');
}, 2000);
