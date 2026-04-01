import { Readable, Writable, Duplex, Transform } from 'node:stream';

async function test_isolated() {
    console.log('[1] Creating upper');
    const upper = new Transform({
        transform(chunk, encoding, callback) {
            callback(null, chunk.toString().toUpperCase());
        }
    });

    console.log('[1.1] Setting up listeners on upper');
    upper.on('end', () => console.log('upper ON end'));
    upper.on('finish', () => console.log('upper ON finish'));

    const dest = new Writable({
        write(chunk, encoding, callback) {
            callback();
        }
    });
    console.log('[1.2] Setting up listeners on dest');
    dest.on('finish', () => console.log('dest ON finish'));

    console.log('[2] Piping upper to dest');
    upper.pipe(dest);

    console.log('[3] Calling upper.end()');
    try {
        upper.end();
        console.log('[4] End returned successfully');
    } catch(e) {
        console.log('[4] End threw', e);
    }

    console.log('[5] test_isolated finished');
}
test_isolated();
