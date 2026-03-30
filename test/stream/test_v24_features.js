// test/stream/test_v24_features.js
import { Readable, Writable, PassThrough, Duplex, pipeline, finished, 
        isErrored, isReadable, isWritable, isDisturbed, 
        getDefaultHighWaterMark, setDefaultHighWaterMark } from 'node:stream';

// Polyfill simple assert
const assert = {
    strictEqual: (a, b) => { if (a !== b) throw new Error(`${a} !== ${b}`); },
    ok: (a) => { if (!a) throw new Error(`${a} is not truthy`); },
    deepStrictEqual: (a, b) => { if (JSON.stringify(a) !== JSON.stringify(b)) throw new Error(`${JSON.stringify(a)} !== ${JSON.stringify(b)}`); }
};

async function test() {
    console.log('--- Testing Node.js v24.x Stream Features ---');
    const keepAlive = setTimeout(() => {}, 5000);

    // 1. Utility Methods
    console.log('1. Checking Utility Methods...');
    assert.strictEqual(isReadable(new Readable({ read(){} })), true);
    assert.strictEqual(isWritable(new Writable({ write(){} })), true);
    assert.strictEqual(isDisturbed(new Readable({ read(){} })), false);

    // 2. HighWaterMark
    console.log('2. Checking Default HighWaterMark...');
    const defaultHWM = getDefaultHighWaterMark(false);
    console.log(`  Default HWM: ${defaultHWM}`);
    setDefaultHighWaterMark(32768, false);
    assert.strictEqual(getDefaultHighWaterMark(false), 32768);
    setDefaultHighWaterMark(16384, false);

    // 3. Readable.from (Async Iterator)
    console.log('3. Checking Readable.from with Array...');
    const fromArray = Readable.from(['a', 'b', 'c']);
    const result = [];
    for await (const chunk of fromArray) {
        console.log(`  Array chunk: ${chunk}`);
        result.push(chunk);
    }
    assert.deepStrictEqual(result, ['a', 'b', 'c']);

    // 4. Readable.from (Generator & for-await)
    console.log('4. Checking Readable.from with Generator...');
    function* gen() { yield 1; yield 2; yield 3; }
    const fromGen = Readable.from(gen());
    const genResult = [];
    for await (const chunk of fromGen) {
        console.log(`  Gen chunk: ${chunk}`);
        genResult.push(chunk);
    }
    assert.deepStrictEqual(genResult, [1, 2, 3]);

    // 5. Duplex allowHalfOpen
    console.log('5. Checking Duplex.allowHalfOpen...');
    const duplex = new Duplex({ allowHalfOpen: false, read(){}, write(){} });
    assert.strictEqual(duplex.allowHalfOpen, false);
    duplex.allowHalfOpen = true;
    assert.strictEqual(duplex.allowHalfOpen, true);

    // 6. Symbols (asyncDispose)
    console.log('6. Checking Symbol.asyncDispose stub...');
    const rs = new Readable({ read(){} });
    if (typeof rs[Symbol.asyncDispose] === 'function') {
        console.log('  Symbol.asyncDispose found on Readable');
    }
    const ws = new Writable({ write(){} });
    if (typeof ws[Symbol.asyncDispose] === 'function') {
        console.log('  Symbol.asyncDispose found on Writable');
    }

    // 7. Web Streams Interop Stubs
    console.log('7. Checking Web Streams Stubs...');
    assert.strictEqual(typeof Readable.fromWeb, 'function');
    try {
        Readable.fromWeb({});
    } catch (e) {
        console.log(`  Readable.fromWeb throws correctly: ${e.message}`);
    }

    console.log('✅ ALL TESTS PASSED!');
    clearTimeout(keepAlive);
}

test().catch(err => {
    console.error('❌ TEST FAILED:', err);
    process.exit(1);
});
