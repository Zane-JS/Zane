// Minimal HTTP test - no background thread
import http from 'node:http';

console.log('Creating server...');

const server = http.createServer((req, res) => {
    console.log('Request!');
    res.end('OK\n');
});

console.log('Calling listen...');

try {
    server.listen(3000, '127.0.0.1', () => {
        console.log('Listen callback!');
    });
    console.log('Listen returned');
} catch (e) {
    console.log('Error:', e);
}

console.log('Waiting 2 seconds...');
setTimeout(() => {
    console.log('Timeout fired');
}, 2000);
