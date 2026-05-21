# DZFoot GF Server — Deploy on Docker Desktop (Windows)
# Usage: .\deploy.ps1 [build|run|logs|stop]

param(
    [Parameter()]
    [ValidateSet("build", "run", "logs", "stop", "clean")]
    [string]$Action = "build"
)

$ErrorActionPreference = "Stop"

switch ($Action) {
    "build" {
        Write-Host "=== Building DZFoot GF Server Docker image ===" -ForegroundColor Cyan
        docker build -t dzfoot-gf-server:latest .
        if ($LASTEXITCODE -eq 0) {
            Write-Host "Build succeeded. Image: dzfoot-gf-server:latest" -ForegroundColor Green
        } else {
            Write-Host "Build FAILED. Check Docker Desktop is running (WSL2 backend)." -ForegroundColor Red
            exit 1
        }
    }

    "run" {
        Write-Host "=== Running GF Server (60s test match) ===" -ForegroundColor Cyan
        docker run --rm --name gf-server-test `
            -e GFOOTBALL_DATA_DIR=/app/data `
            -e GFOOTBALL_FONT=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf `
            dzfoot-gf-server:latest `
            --room-id=match-test `
            --team-a=algeria `
            --team-b=morocco `
            --stadium=default-stadium `
            --duration=60 `
            --broadcast-hz=20
    }

    "logs" {
        docker logs -f gf-server-test
    }

    "stop" {
        docker stop gf-server-test 2>$null
        Write-Host "Container stopped" -ForegroundColor Yellow
    }

    "clean" {
        docker rm -f gf-server-test 2>$null
        docker rmi dzfoot-gf-server:latest 2>$null
        Write-Host "Cleaned up" -ForegroundColor Yellow
    }
}
