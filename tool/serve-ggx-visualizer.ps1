$root = "D:\YYBWorkSpace\GitHub\VulkanLearn"
$file = Join-Path $root "tool\ggx-importance-visualizer.html"

$listener = [System.Net.HttpListener]::new()
$listener.Prefixes.Add("http://localhost:8123/")
$listener.Start()

Write-Output "Serving http://localhost:8123/"

try {
    while ($listener.IsListening) {
        $context = $listener.GetContext()
        $response = $context.Response
        $requestPath = $context.Request.Url.AbsolutePath

        if ($requestPath -eq "/" -or $requestPath -eq "/index.html") {
            $bytes = [System.IO.File]::ReadAllBytes($file)
            $response.ContentType = "text/html; charset=utf-8"
            $response.ContentLength64 = $bytes.Length
            $response.OutputStream.Write($bytes, 0, $bytes.Length)
        }
        else {
            $response.StatusCode = 404
            $msg = [System.Text.Encoding]::UTF8.GetBytes("Not Found")
            $response.OutputStream.Write($msg, 0, $msg.Length)
        }

        $response.OutputStream.Close()
    }
}
finally {
    $listener.Stop()
    $listener.Close()
}
