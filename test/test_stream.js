import stream from "node:stream";
console.log("Exports:");
for (const key of Object.keys(stream)) {
    console.log(`- ${key}: typeof ${typeof stream[key]}`);
}
console.log("Promises API:", !!stream.promises);
