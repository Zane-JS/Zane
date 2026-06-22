process.stdout.write('Hello from stdout.write! ');
process.stdout.write('It works without newlines.\n');

process.stderr.write('This is an error message in stderr.\n');

console.log('Old directory:', process.cwd());
process.chdir('..');
console.log('New directory:', process.cwd());
process.chdir('Zane-app'); // Go back
console.log('Back to:', process.cwd());
