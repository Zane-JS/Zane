async function test() {
    console.log("Testing dynamic import()...");
    try {
        const fs = await import("node:fs");
        console.log("Imported node:fs successfully!");
        console.log("fs.readFileSync is a function:", typeof fs.readFileSync === 'function');
        
        const { readFileSync } = await import("node:fs");
        console.log("Destructured readFileSync is a function:", typeof readFileSync === 'function');
        
        console.log("✅ Dynamic import() test passed!");
    } catch (err) {
        console.error("❌ Dynamic import() test failed:", err);
        process.exit(1);
    }
}

test();
