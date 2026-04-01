// test/stream/test_async_fail.js
import { Readable } from 'node:stream';

async function test() {
    console.log('Testing AsyncIterator...');
    const items = ['a', 'b', 'c'];
    const stream = Readable.from(items);
    
    console.log('  Testing iteration...');
    for await (const chunk of stream) {
        console.log(`    Chunk: ${chunk}`);
    }
    console.log('  Iteration DONE.');

    console.log('  Testing asyncDispose...');
    if (typeof stream[Symbol.asyncDispose] === 'function') {
        await stream[Symbol.asyncDispose]();
        console.log('  asyncDispose DONE.');
    } else {
        console.log('  asyncDispose missing!');
    }
}

test().catch(err => {
    console.error('FAIL:', err);
    process.exit(1);
});
