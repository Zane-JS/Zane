import { Readable, Writable, Duplex, Transform, pipeline, finished, compose } from 'node:stream';

async function test3() {
    console.log('Test 3: Transform & pipeline (Callback)...');
    console.log('source creating');
    const source = Readable.from(['hello']);
    source.on('data', () => console.log('source data'));
    source.on('end', () => console.log('source end'));

    console.log('upper creating');
    const upper = new Transform({
        transform(chunk, encoding, callback) {
            console.log('upper transform chunk', chunk.toString());
            callback(null, chunk.toString().toUpperCase());
        }
    });

    let result = '';
    console.log('dest creating');
    const dest = new Writable({
        write(chunk, encoding, callback) {
            console.log('dest write chunk', chunk.toString());
            result += chunk.toString();
            callback();
        }
    });

    dest.on('finish', () => console.log('dest finish'));
    dest.on('close', () => console.log('dest close'));

    console.log('starting pipeline');
    await new Promise((resolve, reject) => {
        pipeline(source, upper, dest, (err) => {
            console.log('pipeline callback, err=', err);
            if (err) reject(err);
            else resolve();
        });
    });
    console.log('Test 3 Result:', result);
    if (result !== 'HELLO') throw new Error('Failed');
}

test3().then(() => console.log('TEST 3 OK')).catch(e => { console.error(e); process.exit(1); });
