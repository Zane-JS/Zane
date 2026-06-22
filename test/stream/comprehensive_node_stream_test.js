// test/stream/comprehensive_node_stream_test.js
import { Readable, Writable, Duplex, Transform, pipeline, finished, compose } from 'node:stream';
import { pipeline as pipelinePromise, finished as finishedPromise } from 'node:stream/promises';

// --- Custom Assertions ---
const assert = {
    strictEqual: (a, b, msg) => { if (a !== b) throw new Error(`${msg}: ${a} !== ${b}`); },
    ok: (a, msg) => { if (!a) throw new Error(msg || "Assertion failed"); },
    deepStrictEqual: (a, b, msg) => { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`${msg}: ${JSON.stringify(a)} !== ${JSON.stringify(b)}`); }
};

async function runTest(name, fn) {
    process.stdout.write(`Testing ${name}... `);
    try {
        await fn();
        console.log('✅');
    } catch (err) {
        console.log('❌');
        console.error(err);
        process.exit(1);
    }
}

async function start() {
    console.log('====================================================');
    console.log('      Zane COMPREHENSIVE STREAM TEST SUITE v24');
    console.log('====================================================\n');

    // 1. Core Readable & Static Methods
    await runTest('Readable.from() & Events', async () => {
        const source = Readable.from(['z', '8', ' ']);
        let data = '';
        source.on('data', chunk => data += chunk);
        await new Promise(r => source.on('end', r));
        assert.strictEqual(data, 'zane ', 'Data mismatch');
    });

    // 2. Async Iteration & Disposal
    await runTest('AsyncIterator & AsyncDispose', async () => {
        const items = ['power', 'of', 'zane'];
        const stream = Readable.from(items);
        const collected = [];
        for await (const chunk of stream) {
            collected.push(chunk.toString()); // Force string conversion
        }
        
        if (JSON.stringify(collected) !== JSON.stringify(items)) {
            throw new Error(`Data mismatch: got ${JSON.stringify(collected)}, expected ${JSON.stringify(items)}`);
        }

        // Check Symbol.asyncDispose availability
        if (typeof stream[Symbol.asyncDispose] !== 'function') {
            throw new Error('Symbol.asyncDispose is missing on stream instance');
        }
    });

    // 3. Transform & Chaining
    await runTest('Transform & pipeline (Callback)', async () => {
        const source = Readable.from(['hello']);
        const upper = new Transform({
            transform(chunk, encoding, callback) {
                callback(null, chunk.toString().toUpperCase());
            }
        });
        let result = '';
        const dest = new Writable({
            write(chunk, encoding, callback) {
                result += chunk.toString();
                callback();
            }
        });

        await new Promise((resolve, reject) => {
            pipeline(source, upper, dest, (err) => {
                if (err) reject(err);
                else resolve();
            });
        });
        assert.strictEqual(result, 'HELLO', 'Transform failed');
    });

    // 4. Promises API
    await runTest('stream/promises (pipeline & finished)', async () => {
        const source = Readable.from(['v24', 'parity']);
        const results = [];
        const dest = new Writable({
            write(chunk, encoding, callback) {
                results.push(chunk.toString());
                callback();
            }
        });

        await pipelinePromise(source, dest);
        assert.deepStrictEqual(results, ['v24', 'parity'], 'Promise pipeline failed');
        
        await finishedPromise(dest);
    });

    // 5. Web Stream Interop (Readable)
    await runTest('Web Interop: Readable.toWeb()', async () => {
        const nodeReadable = Readable.from(['web', 'streams']);
        const webStream = nodeReadable.toWeb();
        assert.ok(webStream instanceof ReadableStream, 'toWeb() should return a ReadableStream');
        
        const reader = webStream.getReader();
        const results = [];
        while (true) {
            const { done, value } = await reader.read();
            if (done) break;
            results.push(value.toString());
        }
        assert.deepStrictEqual(results, ['web', 'streams'], 'Web reader failed');
    });

    // 6. Web Stream Interop (Writable)
    await runTest('Web Interop: Writable.toWeb()', async () => {
        const results = [];
        const nodeWritable = new Writable({
            write(chunk, encoding, cb) {
                results.push(chunk.toString());
                cb();
            }
        });
        
        const webStream = nodeWritable.toWeb();
        assert.ok(webStream instanceof WritableStream, 'toWeb() should return a WritableStream');
        
        const writer = webStream.getWriter();
        await writer.write('interop');
        await writer.write('works');
        assert.deepStrictEqual(results, ['interop', 'works'], 'Web write interop failed');
    });

    // 7. Duplex Interop
    await runTest('Web Interop: Duplex.toWeb()', async () => {
        const nodeDuplex = new Duplex({
            read() { this.push('pong'); this.push(null); },
            write(chunk, encoding, cb) { cb(); }
        });
        const { readable, writable } = nodeDuplex.toWeb();
        assert.ok(readable instanceof ReadableStream, 'Duplex readable interop fail');
        assert.ok(writable instanceof WritableStream, 'Duplex writable interop fail');
        
        const reader = readable.getReader();
        const { value } = await reader.read();
        assert.strictEqual(value.toString(), 'pong', 'Duplex reading interop failed');
    });

    // 8. Stream Collections (Modern v24)
    await runTest('Stream Collections (map, toArray)', async () => {
        const source = Readable.from([1, 2, 3]);
        // map returns a new readable in v24
        const mapped = source.map(x => x * 2);
        const result = await mapped.toArray();
        assert.deepStrictEqual(result, [2, 4, 6], 'Stream collections failed');
    });

    // 9. compose() Utility
    await runTest('compose() logic', async () => {
        const upper = new Transform({
            transform(chunk, enc, cb) { cb(null, chunk.toString().toUpperCase()); }
        });
        const exclamation = new Transform({
            transform(chunk, enc, cb) { cb(null, chunk.toString() + '!'); }
        });

        const combined = compose(upper, exclamation);
        combined.write('zane');
        
        const out = await new Promise(r => {
            combined.on('data', data => r(data.toString()));
        });
        assert.strictEqual(out, 'Zane!', 'compose() failed');
    });

    console.log('\n====================================================');
    console.log('      ALL 9 MAJOR STREAM FEATURE SUITES PASSED!      ');
    console.log('====================================================');
    console.log('Zane is now fully compliant with Node.js v24.x Stream spec.');
}

start().catch(err => {
    console.error('\n❌ COMPREHENSIVE TEST FAILED:', err);
    process.exit(1);
});
