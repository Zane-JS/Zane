// Test Z8 HTTP Server with Windows Winsock
import http from 'node:http';

console.log('=== Z8 HTTP Server Test ===');
console.log('Backend: Windows Winsock (native)');
console.log('');

const server = http.createServer((req, res) => {
    console.log(`${req.method} ${req.url}`);
    console.log('Headers:', req.headers);
    
    res.writeHead(200, {
        'Content-Type': 'text/plain',
        'X-Powered-By': 'Z8-Winsock'
    });
    
    res.end('Hello from Z8 with native Windows Winsock!\n');
});

server.listen(3000, '127.0.0.1', () => {
    console.log('✓ Server listening on http://127.0.0.1:3000');
    console.log('');
    console.log('Test with:');
    console.log('  curl http://127.0.0.1:3000');
    console.log('  or open in browser');
    console.log('');
    console.log('Press Ctrl+C to stop');
});
