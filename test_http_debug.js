// Debug HTTP server
import http from 'node:http';

console.log('1. Creating server...');

const server = http.createServer((req, res) => {
    console.log('REQUEST RECEIVED!');
    res.writeHead(200);
    res.end('Hello!\n');
});

console.log('2. Server created:', typeof server);
console.log('3. Server.listen:', typeof server.listen);

console.log('4. Calling listen...');
server.listen(3000, '127.0.0.1', () => {
    console.log('5. LISTEN CALLBACK FIRED!');
    console.log('6. Server is running on http://127.0.0.1:3000');
});

console.log('7. After listen() call');

// Keep alive with setInterval
console.log('8. Creating keep-alive interval...');
setInterval(() => {
    console.log('9. Keep-alive tick...');
}, 2000);

console.log('10. Script end');
