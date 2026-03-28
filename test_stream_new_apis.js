import stream from 'node:stream';

console.log('=== Testing New Stream APIs ===\n');

// Test 1: duplexPair()
console.log('1. Testing duplexPair()...');
try {
    const [sideA, sideB] = stream.duplexPair();
    console.log('   ✓ duplexPair created');
    
    sideA.on('data', (chunk) => {
        console.log('   ✓ Side A received:', chunk.toString());
    });
    
    sideB.write('Hello from B');
    console.log('   ✓ Data written to side B');
} catch (e) {
    console.log('   ✗ Error:', e.message);
}

// Test 2: Additional properties
console.log('\n2. Testing additional properties...');
try {
    const readable = new stream.Readable();
    console.log('   - readableAborted:', readable.readableAborted);
    console.log('   - readableDidRead:', readable.readableDidRead);
    console.log('   ✓ Readable properties accessible');
    
    const writable = new stream.Writable();
    console.log('   - writableAborted:', writable.writableAborted);
    console.log('   ✓ Writable properties accessible');
} catch (e) {
    console.log('   ✗ Error:', e.message);
}

// Test 3: readable.compose()
console.log('\n3. Testing readable.compose()...');
try {
    const readable = stream.Readable.from([1, 2, 3]);
    const transform = new stream.Transform({
        transform(chunk, encoding, callback) {
            callback(null, chunk * 2);
        }
    });
    
    const composed = readable.compose(transform);
    console.log('   ✓ Composed stream created');
    console.log('   - Has readable property:', typeof composed.readable);
    console.log('   - Has writable property:', typeof composed.writable);
} catch (e) {
    console.log('   ✗ Error:', e.message);
}

// Test 4: readable.iterator()
console.log('\n4. Testing readable.iterator()...');
try {
    const readable = stream.Readable.from(['a', 'b', 'c']);
    const iterator = readable.iterator({ destroyOnReturn: false });
    console.log('   ✓ Iterator created');
    console.log('   - Has next method:', typeof iterator.next === 'function');
    console.log('   - Has return method:', typeof iterator.return === 'function');
} catch (e) {
    console.log('   ✗ Error:', e.message);
}

console.log('\n=== New Stream APIs Tests Complete ===');
