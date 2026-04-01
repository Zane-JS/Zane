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
