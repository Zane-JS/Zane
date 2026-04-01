// Ultra simple HTTP test - no timers
import http from 'node:http';

console.log('Creating server...');

const server = http.createServer((req, res) => {
    console.log('Request received!');
    res.writeHead(200);
    res.end('OK\n');
});

console.log('Calling listen...');
server.listen(3000, '127.0.0.1', () => {
    console.log('Listen callback executed');
});

console.log('Script done');
