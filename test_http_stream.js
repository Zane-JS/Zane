import http from 'node:http';

const server = http.createServer((req, res) => {
    console.log(`\n=== New Request ===`);
    console.log(`Method: ${req.method}`);
    console.log(`URL: ${req.url}`);
    console.log(`HTTP Version: ${req.httpVersion} (Major: ${req.httpVersionMajor}, Minor: ${req.httpVersionMinor})`);
    console.log(`Parsing Complete: ${req.complete}`);
    
    console.log(`\n--- Parsed Headers ---`);
    console.log(JSON.stringify(req.headers, null, 2));
    
    console.log(`\n--- Raw Headers (from Node) ---`);
    console.log(req.rawHeaders);
    
    // Testing Response Headers
    res.setHeader('X-Custom-Header', 'Zane-V8');
    res.setHeader('Content-Type', 'application/json');
    
    // Test getHeader
    console.log(`\nGet Content-Type: ${res.getHeader('Content-Type')}`);
    
    // Test removeHeader
    res.setHeader('X-Temp-Header', 'to-be-removed');
    res.removeHeader('X-Temp-Header');
    console.log(`Get X-Temp-Header after removal: ${res.getHeader('X-Temp-Header') || 'null'}`);
    
    let chunkCount = 0;
    req.on('data', (chunk) => {
        chunkCount++;
    });
    
    req.on('end', () => {
        res.writeHead(200, { 'X-Status': 'complete' });
        res.end(JSON.stringify({ 
            success: true, 
            chunks: chunkCount, 
            message: 'Streaming and headers test passed!' 
        }));
    });
});

server.listen(3000, '127.0.0.1', () => {
    console.log('Server started on http://127.0.0.1:3000');
});
