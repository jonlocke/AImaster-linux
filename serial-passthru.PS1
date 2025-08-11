powershell -ExecutionPolicy Bypass -File .\serial-passthru.PS1
$busid = (usbipd list | Out-String | ForEach-Object { ($_ -split "`n") | ForEach-Object { if ($_ -match "^\d-\d+") { $_.Split()[0] } } } | Out-GridView -Title "Select USB device to attach" -PassThru); if ($busid) { usbipd bind --busid $busid; usbipd attach --busid $busid --wsl Ubuntu-22.04 }
