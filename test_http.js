<<<<<<< Updated upstream
// Test Z8 HTTP Server with native IOCP backend
import http from 'node:http';

console.log('=== Z8 HTTP Server Test ===');
console.log('Backend: Windows IOCP (native)');
console.log('');

const server = http.createServer((req, res) => {
    console.log(`${req.method} ${req.url}`);
    console.log('Headers:', req.headers);
    
    res.writeHead(200, {
        'Content-Type': 'text/plain',
        'X-Powered-By': 'Z8-IOCP'
    });
    
    res.end('Hello from Z8 with native Windows IOCP!\n');
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
=======
// Test node:http module
import http from 'node:http';

console.log('Testing node:http module...');
console.log('http.METHODS:', http.METHODS);
console.log('http.STATUS_CODES:', http.STATUS_CODES);

try {
    const server = http.createServer();
    console.log('✓ http.createServer() called (not yet implemented)');
} catch (e) {
    console.log('✓ Expected error:', e.message);
}

console.log('\n✓ node:http module loaded successfully!');
>>>>>>> Stashed changes
