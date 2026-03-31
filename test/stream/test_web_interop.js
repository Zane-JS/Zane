// test/stream/test_web_interop.js
import { Readable } from 'node:stream';

// Polyfill simple assert
const assert = {
    strictEqual: (a, b) => { if (a !== b) throw new Error(`${a} !== ${b}`); },
    ok: (a) => { if (!a) throw new Error(`${a} is not truthy`); },
    deepStrictEqual: (a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`${JSON.stringify(a)} !== ${JSON.stringify(b)}`); }
};

async function test() {
    console.log('--- Testing Node.js Web Stream Interop ---');

    // 1. Check Globals
    console.log('1. Checking Globals...');
    assert.strictEqual(typeof ReadableStream, 'function');
    assert.strictEqual(typeof WritableStream, 'function');

    // 2. Readable.toWeb()
    console.log('2. Testing Readable.toWeb()...');
    const nodeReadable = Readable.from(['hello', 'world']);
    const webStream = nodeReadable.toWeb();
    assert.ok(webStream instanceof ReadableStream);

    const reader = webStream.getReader();
    const chunk1 = await reader.read();
    console.log(`  Chunk 1: ${JSON.stringify(chunk1)}`);
    assert.deepStrictEqual(chunk1, { value: 'hello', done: false });

    const chunk2 = await reader.read();
    console.log(`  Chunk 2: ${JSON.stringify(chunk2)}`);
    assert.deepStrictEqual(chunk2, { value: 'world', done: false });

    const chunk3 = await reader.read();
    console.log(`  Chunk 3: ${JSON.stringify(chunk3)}`);
    assert.strictEqual(chunk3.done, true);

    // 3. Readable.fromWeb()
    console.log('3. Testing Readable.fromWeb()...');
    // We can use the webStream from above or a new one
    const nodeFromWeb = Readable.fromWeb(webStream);
    assert.ok(nodeFromWeb instanceof Readable);
    
    // In our minimal implementation, fromWeb will just pull from the reader
    // Let's test with a fresh Web Stream wrapping a Node Stream
    const source = Readable.from(['web', 'streams']);
    const ws = source.toWeb();
    const nr = Readable.fromWeb(ws);
    
    const results = [];
    for await (const chunk of nr) {
        console.log(`  From Web chunk: ${chunk}`);
        results.push(chunk);
    }
    assert.deepStrictEqual(results, ['web', 'streams']);

    console.log('✅ WEB INTEROP TESTS PASSED!');
}

test().catch(err => {
    console.error('❌ TEST FAILED:', err);
    process.exit(1);
});
