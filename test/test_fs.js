import { readFileSync, writeFileSync, existsSync } from 'node:fs';

console.log('Testing node:fs module...');

const testFile = 'zane_test_file.txt';
const content = 'Hello from Zane V8 (Zane)!';

console.log('Writing to file:', testFile);
writeFileSync(testFile, content);

if (existsSync(testFile)) {
    console.log('File exists, reading back...');
    const data = readFileSync(testFile);
    console.log('Content:', data);
    
    if (data === content) {
        console.log('✅ node:fs test passed!');
    } else {
        console.error('❌ node:fs test failed: Data mismatch');
    }
} else {
    console.error('❌ node:fs test failed: File not found after write');
}
