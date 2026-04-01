// test/stream/test_writable_web_interop.js
import { Writable } from 'node:stream';

// Polyfill simple assert
const assert = {
    strictEqual: (a, b) => { if (a !== b) throw new Error(`${a} !== ${b}`); },
    ok: (a) => { if (!a) throw new Error(`${a} is not truthy`); },
    deepStrictEqual: (a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`${JSON.stringify(a)} !== ${JSON.stringify(b)}`); }
};

async function test() {
    console.log('--- Testing Node.js Writable Web Stream Interop ---');

    // 1. Writable.toWeb()
    console.log('1. Testing Writable.toWeb()...');
    const results = [];
    const nodeWritable = new Writable({
        write(chunk, encoding, callback) {
            console.log(`  Node Write: ${chunk}`);
            results.push(chunk.toString());
            callback();
        }
    });

    const webWritable = nodeWritable.toWeb();
    assert.ok(webWritable instanceof WritableStream);

    const writer = webWritable.getWriter();
    await writer.write('hello');
    await writer.write('world');
    
    assert.deepStrictEqual(results, ['hello', 'world']);

    // 2. Writable.fromWeb()
    console.log('2. Testing Writable.fromWeb()...');
    // Wrap the same web stream back
    const nodeFromWeb = Writable.fromWeb(webWritable);
    assert.ok(nodeFromWeb instanceof Writable);
    
    // Test writing to the node stream which wraps web stream
    nodeFromWeb.write('from node to web');
    
    // In our implementation, results should have the new value
    // Wait a bit for async pipe
    await new Promise(r => setTimeout(r, 100));
    assert.ok(results.includes('from node to web'));

    console.log('✅ WRITABLE INTEROP TESTS PASSED!');
}

test().catch(err => {
    console.error('❌ TEST FAILED:', err);
    process.exit(1);
});
