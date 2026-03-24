import stream from "node:stream";

const missing = [
  "isErrored",
  "isReadable",
  "isDisturbed",
  "destroy"
];

const found = [];
const notFound = [];

for (const key of missing) {
    if (stream[key] === undefined) {
        notFound.push(key);
    } else {
        found.push(key);
    }
}

console.log("Missing exports:");
console.log(notFound.join("\n"));

console.log("\nFound exports:");
console.log(found.join("\n"));
