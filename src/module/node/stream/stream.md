# Stream

The `stream` module provides an API for implementing the stream interface. Streams are an abstract interface for working with streaming data in Z8. All streams are instances of EventEmitter.

```js
import stream from "node:stream";
```

## Types of Streams

There are four fundamental stream types within Z8:

- **Readable**: streams from which data can be read
- **Writable**: streams to which data can be written
- **Duplex**: streams that are both Readable and Writable
- **Transform**: Duplex streams that can modify or transform the data as it is written and read

## Class: stream.Readable

Readable streams are an abstraction for a source from which data is consumed.

### Constructor

```js
new stream.Readable([options]);
```

**Options:**
- `highWaterMark` (number): Buffer level threshold. Default: 16384 (16 KiB)
- `encoding` (string): If specified, buffers will be decoded to strings
- `objectMode` (boolean): Whether this stream should behave as a stream of objects
- `read` (function): Implementation for the `_read()` method

### Methods

#### `readable.read([size])`

Reads data out of the internal buffer and returns it. If no data is available, returns `null`.

#### `readable.push(chunk[, encoding])`

Pushes a chunk of data into the internal buffer. Returns `true` if additional chunks may continue to be pushed; `false` otherwise.

#### `readable.pause()`

Causes a stream in flowing mode to stop emitting 'data' events, switching out of flowing mode.

#### `readable.resume()`

Causes an explicitly paused Readable stream to resume emitting 'data' events, switching into flowing mode.

#### `readable.isPaused()`

Returns the current operating state of the Readable. Returns `true` if the stream is paused.

#### `readable.pipe(destination[, options])`

Attaches a Writable stream to the readable, causing it to switch automatically into flowing mode and push all of its data to the attached Writable.

#### `readable.unpipe([destination])`

Detaches a Writable stream previously attached using `pipe()`.

#### `readable.unshift(chunk[, encoding])`

Pushes a chunk of data back into the internal buffer. Useful when a stream is being consumed and needs to "un-consume" some data.

#### `readable.wrap(stream)`

Wraps an old-style readable stream (pre-Node.js 0.10) to make it compatible with the current stream API.

#### `readable.destroy([error])`

Destroys the stream. Optionally emits an 'error' event and emits a 'close' event.

#### `readable.setEncoding(encoding)`

Sets the character encoding for data read from the Readable stream.

### Collection Methods

The following methods allow treating a Readable stream as an iterable collection:

#### `readable.map(fn[, options])`

Creates a new stream with the results of calling a provided function on every chunk.

**Status**: Implemented

#### `readable.filter(fn[, options])`

Creates a new stream with all chunks that pass the test implemented by the provided function.

**Status**: Implemented

#### `readable.forEach(fn[, options])`

Calls a function for each chunk in the stream. Returns a Promise that resolves when complete.

**Status**: Implemented

#### `readable.toArray([options])`

Collects all chunks from the stream into an array. Returns a Promise that resolves with the array.

**Status**: Implemented

#### `readable.some(fn[, options])`

Tests whether at least one chunk in the stream passes the test implemented by the provided function. Returns a Promise.

**Status**: Implemented

#### `readable.find(fn[, options])`

Returns the first chunk in the stream that satisfies the provided testing function. Returns a Promise.

**Status**: Implemented

#### `readable.every(fn[, options])`

Tests whether all chunks in the stream pass the test implemented by the provided function. Returns a Promise.

**Status**: Implemented

#### `readable.flatMap(fn[, options])`

Creates a new stream by applying a function to each chunk and flattening the result. If the function returns an array or iterable, each element is emitted separately.

**Status**: Implemented

**Example:**
```js
const readable = stream.Readable.from([1, 2, 3]);
const flatMapped = readable.flatMap(x => [x, x * 2]);
// Emits: 1, 2, 2, 4, 3, 6
```

#### `readable.drop(limit[, options])`

Drops the first `limit` chunks from the stream.

**Status**: Implemented

#### `readable.take(limit[, options])`

Takes the first `limit` chunks from the stream.

**Status**: Implemented

#### `readable.reduce(fn[, initial[, options]])`

Reduces the stream to a single value by executing a reducer function for each chunk. Returns a Promise.

**Status**: Implemented

### Static Methods

#### `stream.Readable.from(iterable[, options])`

Creates a Readable stream from an iterable (array, generator, etc.).

```js
const readable = stream.Readable.from(['hello', 'world']);
```

### Properties

#### `readable.readable`

Is `true` if it is safe to call `readable.read()`.

#### `readable.readableFlowing`

Reflects the current state of a Readable stream:
- `null`: no mechanism for consuming data is provided
- `false`: stream is paused
- `true`: stream is in flowing mode

#### `readable.readableHighWaterMark`

Returns the value of `highWaterMark` passed when creating this Readable.

#### `readable.readableLength`

Contains the number of bytes (or objects) in the queue ready to be read.

#### `readable.readableEncoding`

Returns the encoding property of the Readable stream.

#### `readable.readableEnded`

Becomes `true` when 'end' event is emitted.

#### `readable.readableObjectMode`

Returns `true` if the stream is operating in object mode.

#### `readable.closed`

Is `true` after 'close' has been emitted.

#### `readable.destroyed`

Is `true` after `readable.destroy()` has been called.

#### `readable.errored`

Returns error if the stream has been destroyed with an error.

### Events

- `'data'`: Emitted whenever the stream is relinquishing ownership of a chunk of data
- `'end'`: Emitted when there is no more data to be consumed from the stream
- `'error'`: Emitted if an error occurred
- `'close'`: Emitted when the stream and any underlying resources have been closed
- `'pause'`: Emitted when `pause()` is called
- `'resume'`: Emitted when `resume()` is called
- `'readable'`: Emitted when there is data available to be read from the stream

## Class: stream.Writable

Writable streams are an abstraction for a destination to which data is written.

### Constructor

```js
new stream.Writable([options]);
```

**Options:**
- `highWaterMark` (number): Buffer level threshold. Default: 16384 (16 KiB)
- `decodeStrings` (boolean): Whether to encode strings to Buffers before passing to `_write()`
- `objectMode` (boolean): Whether this stream should behave as a stream of objects
- `write` (function): Implementation for the `_write()` method

### Methods

#### `writable.write(chunk[, encoding][, callback])`

Writes some data to the stream. Returns `false` if the stream wishes for the calling code to wait for the 'drain' event before continuing to write additional data.

#### `writable.end([chunk][, encoding][, callback])`

Signals that no more data will be written to the Writable.

#### `writable.cork()`

Forces all written data to be buffered in memory. The buffered data will be flushed when `uncork()` or `end()` is called.

#### `writable.uncork()`

Flushes all data buffered since `cork()` was called.

#### `writable.destroy([error])`

Destroys the stream. Optionally emits an 'error' event and emits a 'close' event.

#### `writable.setDefaultEncoding(encoding)`

Sets the default encoding for a Writable stream.

### Properties

#### `writable.writable`

Is `true` if it is safe to call `writable.write()`.

#### `writable.writableHighWaterMark`

Returns the value of `highWaterMark` passed when creating this Writable.

#### `writable.writableLength`

Contains the number of bytes (or objects) in the queue ready to be written.

#### `writable.writableObjectMode`

Returns `true` if the stream is operating in object mode.

#### `writable.writableCorked`

Number of times `uncork()` needs to be called to fully uncork the stream.

#### `writable.writableEnded`

Is `true` after `end()` has been called.

#### `writable.writableFinished`

Is set to `true` immediately before the 'finish' event is emitted.

#### `writable.writableNeedDrain`

Is `true` if the stream's buffer has been full and stream will emit 'drain'.

#### `writable.closed`

Is `true` after 'close' has been emitted.

#### `writable.destroyed`

Is `true` after `writable.destroy()` has been called.

#### `writable.errored`

Returns error if the stream has been destroyed with an error.

### Events

- `'drain'`: Emitted when it is appropriate to resume writing data to the stream
- `'finish'`: Emitted after `end()` has been called and all data has been flushed
- `'error'`: Emitted if an error occurred while writing
- `'close'`: Emitted when the stream and any underlying resources have been closed
- `'pipe'`: Emitted when `pipe()` is called on a readable stream
- `'unpipe'`: Emitted when `unpipe()` is called on a Readable stream

## Class: stream.Duplex

Duplex streams are streams that implement both the Readable and Writable interfaces.

### Constructor

```js
new stream.Duplex([options]);
```

Inherits all methods and properties from both Readable and Writable.

### Static Methods

#### `stream.Duplex.from(src)`

Creates a Duplex stream from various sources (readable stream, writable stream, web streams, iterables, etc.).

**Status**: Implemented (basic functionality - creates Duplex from iterables)

## Class: stream.Transform

Transform streams are Duplex streams where the output is computed from the input.

### Constructor

```js
new stream.Transform([options]);
```

**Options:**
- `transform` (function): Implementation for the `_transform()` method
- `flush` (function): Implementation for the `_flush()` method

### Example

```js
const { Transform } = require('node:stream');

const upperCaseTransform = new Transform({
  transform(chunk, encoding, callback) {
    this.push(chunk.toString().toUpperCase());
    callback();
  }
});
```

## Class: stream.PassThrough

PassThrough is a trivial implementation of a Transform stream that simply passes the input bytes across to the output. Its purpose is mainly for examples and testing, but there are some use cases where it can be useful as a building block for novel sorts of streams.

### Constructor

```js
new stream.PassThrough([options]);
```

**Status**: Implemented (write and end methods emit data and end events)

### Example

```js
const { PassThrough } = require('node:stream');
const pass = new PassThrough();

pass.on('data', (chunk) => {
  console.log('Data:', chunk.toString());
});

pass.write('hello ');
pass.write('world');
pass.end();
```

## Utility Functions

### `stream.pipeline(source[, ...transforms], destination[, callback])`

Pipes between streams forwarding errors and properly cleaning up. Calls the callback when the pipeline is complete.

```js
const { pipeline } = require('node:stream');
const fs = require('node:fs');
const zlib = require('node:zlib');

pipeline(
  fs.createReadStream('input.txt'),
  zlib.createGzip(),
  fs.createWriteStream('input.txt.gz'),
  (err) => {
    if (err) {
      console.error('Pipeline failed:', err);
    } else {
      console.log('Pipeline succeeded');
    }
  }
);
```

### `stream.finished(stream[, options], callback)`

Gets notified when a stream is no longer readable, writable, or has experienced an error or premature close event.

```js
const { finished } = require('node:stream');
const fs = require('node:fs');

const rs = fs.createReadStream('archive.tar');

finished(rs, (err) => {
  if (err) {
    console.error('Stream failed:', err);
  } else {
    console.log('Stream is done reading');
  }
});
```

### `stream.compose(...streams)`

Combines two or more streams into a Duplex stream that writes to the first stream and reads from the last.

```js
const { compose, Transform } = require('node:stream');

const removeSpaces = new Transform({
  transform(chunk, encoding, callback) {
    callback(null, String(chunk).replace(' ', ''));
  }
});

const toUpper = new Transform({
  transform(chunk, encoding, callback) {
    callback(null, String(chunk).toUpperCase());
  }
});

const composed = compose(removeSpaces, toUpper);
```

### `stream.addAbortSignal(signal, stream)`

Attaches an AbortSignal to a readable or writable stream. This lets code control stream destruction using an AbortController.

```js
const { addAbortSignal } = require('node:stream');
const fs = require('node:fs');

const controller = new AbortController();
const read = addAbortSignal(
  controller.signal,
  fs.createReadStream('file.txt')
);

// Later, abort the operation
controller.abort();
```

### `stream.getDefaultHighWaterMark(objectMode)`

Returns the default highWaterMark used by streams. Defaults to 65536 (64 KiB), or 16 for objectMode.

### `stream.setDefaultHighWaterMark(objectMode, value)`

Sets the default highWaterMark used by streams.

### `stream.isErrored(stream)`

Returns whether the stream has encountered an error.

### `stream.isReadable(stream)`

Returns whether the stream is readable.

### `stream.isDisturbed(stream)`

Returns whether the stream has been read from or canceled.

### `stream.destroy(stream)`

Destroys the given stream.

## Promises API

The `stream/promises` API provides Promise-based versions of stream utilities.

```js
import { pipeline, finished } from "node:stream/promises";
```

### `stream.promises.pipeline(source[, ...transforms], destination[, options])`

Returns a Promise that fulfills when the pipeline is complete.

```js
const { pipeline } = require('node:stream/promises');
const fs = require('node:fs');
const zlib = require('node:zlib');

async function run() {
  await pipeline(
    fs.createReadStream('input.txt'),
    zlib.createGzip(),
    fs.createWriteStream('input.txt.gz')
  );
  console.log('Pipeline succeeded');
}

run().catch(console.error);
```

### `stream.promises.finished(stream[, options])`

Returns a Promise that fulfills when the stream is no longer readable or writable.

```js
const { finished } = require('node:stream/promises');
const fs = require('node:fs');

const rs = fs.createReadStream('archive.tar');

async function run() {
  await finished(rs);
  console.log('Stream is done reading');
}

run().catch(console.error);
rs.resume(); // Drain the stream
```

## Implementation Notes

### Object Mode

Streams can operate in "object mode" where they work with JavaScript values other than strings and Buffers. Set `objectMode: true` in the constructor options.

### Buffering

Both Writable and Readable streams will store data in an internal buffer. The amount of data buffered depends on the `highWaterMark` option.

### Flowing vs Paused Mode

Readable streams operate in one of two modes:
- **Flowing mode**: Data is read automatically and provided as quickly as possible
- **Paused mode**: `read()` must be called explicitly to read chunks of data

## Z8-Specific Notes

- All stream implementations follow the Node.js v24.x API specification
- Streams are built on top of V8's EventEmitter
- The implementation uses Z8's TaskQueue for asynchronous operations
- Memory management follows Z8's coding guidelines with proper cleanup on destruction
