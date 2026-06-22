import util from 'node:util';

console.log('--- util.format ---');
console.log(util.format('Hello %s', 'Zane'));
console.log(util.format('%d + %d = %d', 1, 2, 3));
console.log(util.format('JSON: %j', { a: 1 }));

console.log('\n--- util.types ---');
console.log('isDate(new Date()):', util.types.isDate(new Date()));
console.log('isRegExp(/abc/):', util.types.isRegExp(/abc/));
console.log('isPromise(Promise.resolve()):', util.types.isPromise(Promise.resolve()));
console.log('isAsyncFunction(async () => {}):', util.types.isAsyncFunction(async () => {}));
console.log('isUint8Array(new Uint8Array()):', util.types.isUint8Array(new Uint8Array()));
console.log('isBoxedPrimitive(new Number(1)):', util.types.isBoxedPrimitive(new Number(1)));
console.log('isBoxedPrimitive(1):', util.types.isBoxedPrimitive(1));

console.log('\n--- util.promisify ---');
const readFile = (path, cb) => {
    setTimeout(() => cb(null, `Content of ${path}`), 10);
};
const readFileAsync = util.promisify(readFile);
readFileAsync('test.txt').then(content => {
    console.log('promisify result:', content);
});

console.log('\n--- util.callbackify ---');
const asyncFunc = async (val) => {
    return `Async result: ${val}`;
};
const callbackFunc = util.callbackify(asyncFunc);
callbackFunc('hello', (err, result) => {
    console.log('callbackify result:', result);
});

console.log('\n--- util.inherits ---');
function Parent() {}
Parent.prototype.sayHi = function() { console.log('Hi from Parent'); };
function Child() {}
util.inherits(Child, Parent);
const child = new Child();
console.log('child instance of Parent:', child instanceof Parent);
if (child.sayHi) child.sayHi();
