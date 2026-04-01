// Final HTTP Server Test
import http from 'node:http';

console.log('🚀 Starting Z8 HTTP Server with native IOCP...\n');

const server = http.createServer((req, res) => {
    console.log(`📨 ${req.method} ${req.url}`);
    
    res.writeHead(200, {
        'Content-Type': 'text/plain',
        'X-Powered-By': 'Z8-IOCP',
        'X-Runtime': 'Z8 JavaScript Runtime'
    });
    
    res.end('Hello from Z8 with native Windows IOCP!\n' +
            'This is a high-performance HTTP server running on native IOCP backend.\n' +
            'No libuv dependency - pure Windows I/O Completion Ports!\n');
});

server.listen(3000, '127.0.0.1', () => {
    console.log('✅ Server is listening on http://127.0.0.1:3000');
    console.log('📊 Backend: Windows IOCP (native)');
    console.log('🔥 Zero libuv dependency');
    console.log('\n💡 Test with: curl http://127.0.0.1:3000');
    console.log('   Or open in your browser\n');
});

// Keep server alive
setInterval(() => {
    // Server heartbeat
}, 5000);
