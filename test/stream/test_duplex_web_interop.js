// test/stream/test_duplex_web_interop.js
import { Duplex } from 'node:stream';

// Polyfill simple assert
const assert = {
    strictEqual: (a, b) => { if (a !== b) throw new Error(`${a} !== ${b}`); },
    ok: (a) => { if (!a) throw new Error(`${a} is not truthy`); },
    deepStrictEqual: (a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`${JSON.stringify(a)} !== ${JSON.stringify(b)}`); }
};

async function test() {
    console.log('--- Testing Node.js Duplex Web Stream Interop ---');

    const results = [];
    const nodeDuplex = new Duplex({
        read(size) {
            this.push('duplex-data');
            this.push(null);
        },
        write(chunk, encoding, callback) {
            console.log(`  Duplex Write: ${chunk}`);
            results.push(chunk.toString());
            callback();
        }
    });

    // 1. Duplex.toWeb()
    console.log('1. Testing Duplex.toWeb()...');
    const webStream = nodeDuplex.toWeb();
    assert.ok(webStream.readable instanceof ReadableStream);
    assert.ok(webStream.writable instanceof WritableStream);

    // Test reading
    const reader = webStream.readable.getReader();
    const { value, done } = await reader.read();
    console.log(`  Read from Duplex: ${value}`);
    assert.strictEqual(value.toString(), 'duplex-data');

    // Test writing
    const writer = webStream.writable.getWriter();
    await writer.write('hello-duplex');
    assert.deepStrictEqual(results, ['hello-duplex']);

    console.log('✅ DUPLEX INTEROP TESTS PASSED!');
}

test().catch(err => {
    console.error('❌ TEST FAILED:', err);
    process.exit(1);
});
