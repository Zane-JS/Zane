import { Transform } from 'node:stream';

async function test_isolated() {
    console.log('[1] Creating upper (Transform)');
    const dest = new Transform({
        transform(chunk, encoding, callback) { callback(null, chunk); }
    });

    console.log('[3] Calling dest.end()');
    try {
        dest.end();
        console.log('[4] End returned successfully');
    } catch(e) {
        console.log('[4] End threw', e);
    }
}
test_isolated();
