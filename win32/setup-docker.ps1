# Install Docker Desktop on Windows CI for Linux container support.
#
# Docker Desktop bundles WSL2 kernel since v4.25, so no separate
# WSL2 install is needed.

$ErrorActionPreference = "Stop"

# Check if Docker is already available
try {
    docker version 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Docker is already installed"
        docker version
        return
    }
} catch {}

$installer = "$env:TEMP\DockerDesktopInstaller.exe"
$dockerUrl = "https://desktop.docker.com/win/main/amd64/Docker%20Desktop%20Installer.exe"

Write-Host "Downloading Docker Desktop..."
Invoke-WebRequest -Uri $dockerUrl -OutFile $installer -UseBasicParsing

Write-Host "Installing Docker Desktop (quiet)..."
Start-Process -Wait -FilePath $installer -ArgumentList "install", "--quiet", "--accept-license"

# Add Docker to PATH for this session
$env:PATH = "C:\Program Files\Docker\Docker\resources\bin;$env:PATH"

Write-Host "Starting Docker Desktop..."
& "C:\Program Files\Docker\Docker\Docker Desktop.exe"

Write-Host "Waiting for Docker daemon to be ready..."
$timeout = 300
$elapsed = 0
while ($true) {
    try {
        docker info 2>&1 | Out-Null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Docker is ready!"
            break
        }
    } catch {}
    Start-Sleep -Seconds 5
    $elapsed += 5
    if ($elapsed -ge $timeout) {
        Write-Error "Docker failed to start within ${timeout}s"
        exit 1
    }
    Write-Host "  Waiting... (${elapsed}s)"
}

docker version
