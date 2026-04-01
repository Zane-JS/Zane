import http from 'node:http';

console.log('Creating HTTP server with llhttp parser...');

const server = http.createServer((req, res) => {
    console.log('Request received:');
    console.log('  Method:', req.method);
    console.log('  URL:', req.url);
    console.log('  Headers:', req.headers);
    
    res.writeHead(200, { 'Content-Type': 'text/plain' });
    res.end('Hello from Z8 with llhttp!\n');
});

server.listen(3000, '127.0.0.1', () => {
    console.log('Server listening on http://127.0.0.1:3000');
    console.log('Test with: curl http://127.0.0.1:3000/test');
});

// Keep alive for 30 seconds
setTimeout(() => {
    console.log('Closing server...');
    server.close();
}, 30000);
