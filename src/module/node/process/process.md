# Process

The `process` module provides information about, and control over, the current Node.js process. While it is available as a global, it can also be accessed explicitly via `import` or `require`:

```js
import process from "node:process";
```

## Properties

### `process.env`

The `process.env` property returns an object containing the user environment.
Zane extension: Zane automatically loads environment variables from a `.env` file in the current working directory if it exists (dotenv functionality).

```js
console.log(process.env.PATH);
console.log(process.env.MY_VAR); // Loads from .env
```

### `process.platform`

Returns a string identifying the operating system platform.

- `'win32'`
- `'linux'`
- `'darwin'`

### `process.arch`

Returns the operating system CPU architecture for which the Zane binary was compiled.

- `'x64'`
- `'ia32'`
- `'arm64'`
- `'arm'`

### `process.version`

The `process.version` property returns the Zane version string.

### `process.versions`

Returns an object listing the version strings of Zane and its dependencies (like V8).

### `process.execPath`

Returns the absolute pathname of the executable that started the process.

### `process.pid`

Returns the PID of the process.

### `process.argv`

An array containing the command-line arguments passed when the Zane process was launched.

### `process.argv0`

Contains a read-only copy of the original value of `argv[0]` passed when Zane was launched.

### `process.title`

The `process.title` property returns or sets the current process title (the name displayed in process managers).

### `process.stdout.isTTY` / `process.stderr.isTTY` / `process.stdin.isTTY`

Returns `true` if the stream is connected to a TTY (terminal).

## Methods

### `process.cwd()`

Returns the current working directory of the process.

### `process.chdir(directory)`

Changes the current working directory of the Node.js process.

### `process.exit([code])`

Terminate the process synchronously with an exit status of `code`. If omitted, exit uses either the 'success' code `0`.

### `process.uptime()`

Returns the number of seconds the current process has been running.

### `process.hrtime([time])`

Returns the current high-resolution real time in a `[seconds, nanoseconds]` tuple `Array`.

### `process.hrtime.bigint()`

Returns the current high-resolution real time in nanoseconds as a `BigInt`.

### `process.kill(pid[, signal])`

Sends a signal to a process.

### `process.memoryUsage()`

Returns an object describing the memory usage of the Zane process.

### `process.cpuUsage([previousValue])`

Returns an object with the user and system CPU time usage of the current process, in microseconds.

### `process.resourceUsage()`

Returns an object describing the resource usage of the current process.

### `process.umask([mask])`

Sets or returns the Node.js process's file mode creation mask.

### `process.nextTick(callback[, ...args])`

Adds `callback` to the "next tick queue". This queue is fully processed after the current operation on the JavaScript stack runs to completion and before the event loop is allowed to continue. (Implemented using V8 microtasks).
