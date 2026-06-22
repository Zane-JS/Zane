console.log("🚀 Start");

setTimeout(() => {
    console.log("⏰ Timer 1 fired after 100ms");
}, 100);

setTimeout((msg) => {
    console.log("⏰ Timer 2 fired after 50ms with arg:", msg);
}, 50, "Zane Performance");

const id = setTimeout(() => {
    console.log("❌ This should NOT print (cleared)");
}, 10);

clearTimeout(id);

console.log("🏁 Script execution finished (event loop starts)");
