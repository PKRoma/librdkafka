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

# Find where Docker was installed and add to PATH
$dockerBin = Get-ChildItem "C:\Program Files\Docker" -Filter "docker.exe" -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
if ($dockerBin) {
    Write-Host "Found docker at $($dockerBin.FullName)"
    $env:PATH = "$($dockerBin.DirectoryName);$env:PATH"
} else {
    # Fallback common paths
    $env:PATH = "C:\Program Files\Docker\Docker\resources\bin;C:\Program Files\Docker\Docker;$env:PATH"
}

# Start Docker via the service (not the GUI app)
Write-Host "Starting Docker service..."
$services = @("com.docker.service", "docker")
foreach ($svc in $services) {
    try {
        Start-Service $svc -ErrorAction Stop
        Write-Host "Started service: $svc"
    } catch {
        Write-Host "Service '$svc' not found or failed to start, trying next..."
    }
}

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
        # Show what services exist for debugging
        Write-Host "Docker-related services:"
        Get-Service *docker* 2>$null | Format-Table -AutoSize
        Write-Error "Docker failed to start within ${timeout}s"
        exit 1
    }
    Write-Host "  Waiting... (${elapsed}s)"
}

docker version
