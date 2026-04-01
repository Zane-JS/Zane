// test/stream/test_stream_promises.js
import { pipeline, finished } from 'node:stream/promises';
import { Readable, Writable } from 'node:stream';

// Polyfill simple assert
const assert = {
    strictEqual: (a, b) => { if (a !== b) throw new Error(`${a} !== ${b}`); },
    ok: (a) => { if (!a) throw new Error(`${a} is not truthy`); },
    deepStrictEqual: (a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`${JSON.stringify(a)} !== ${JSON.stringify(b)}`); }
};

async function test() {
    console.log('--- Testing node:stream/promises ---');

    console.log('1. Testing pipeline()...');
    const source = Readable.from(['hello', 'world']);
    const results = [];
    const dest = new Writable({
        write(chunk, encoding, callback) {
            results.push(chunk.toString());
            callback();
        }
    });

    await pipeline(source, dest);
    assert.deepStrictEqual(results, ['hello', 'world']);
    console.log('   pipeline() successful');

    console.log('2. Testing finished()...');
    const finalStream = Readable.from(['done']);
    finalStream.on('data', () => {}); // Consuming data
    await finished(finalStream);
    console.log('   finished() successful');

    console.log('✅ STREAM PROMISES TESTS PASSED!');
}

test().catch(err => {
    console.error('❌ TEST FAILED:', err);
    process.exit(1);
});
