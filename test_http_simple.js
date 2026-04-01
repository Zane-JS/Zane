// Simple HTTP test
import http from 'node:http';

console.log('Creating HTTP server...');

const server = http.createServer((req, res) => {
    console.log('Request received!');
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('Hello from Z8!\n');
});

console.log('Server object created');
console.log('Server:', server);
console.log('Calling listen...');

server.listen(3000, '127.0.0.1', () => {
    console.log('Listen callback fired!');
    console.log('Server is listening on http://127.0.0.1:3000');
});

console.log('After listen call');
