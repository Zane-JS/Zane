Start-Sleep -Seconds 2
Write-Host "Sending request..."
curl.exe http://127.0.0.1:3000/test
Write-Host "CURL EXITCODE: $LASTEXITCODE"
