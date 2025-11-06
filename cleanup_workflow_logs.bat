@echo off
REM ============================================================================
REM Cleanup GitHub Actions Workflow Logs and History
REM ============================================================================
REM This script deletes local workflow run logs and clears GitHub Actions cache
REM Note: This only clears local data. To delete workflow runs from GitHub,
REM you need to use the GitHub API or web interface.
REM ============================================================================

echo.
echo ============================================================================
echo   GitHub Actions Workflow Cleanup Script
echo ============================================================================
echo.

REM Get the repository root (parent of .github folder)
set "SCRIPT_DIR=%~dp0"
set "REPO_ROOT=%SCRIPT_DIR%"

echo Current directory: %CD%
echo Script directory: %SCRIPT_DIR%
echo Repository root: %REPO_ROOT%
echo.

REM ============================================================================
REM 1. Clear local workflow logs (if any exist)
REM ============================================================================
echo [1/4] Checking for local workflow logs...

if exist "%REPO_ROOT%.github\workflows\logs" (
    echo Found workflow logs directory. Deleting...
    rmdir /s /q "%REPO_ROOT%.github\workflows\logs"
    echo   - Deleted: .github\workflows\logs
) else (
    echo   - No local workflow logs found
)

if exist "%REPO_ROOT%.github\workflows\cache" (
    echo Found workflow cache directory. Deleting...
    rmdir /s /q "%REPO_ROOT%.github\workflows\cache"
    echo   - Deleted: .github\workflows\cache
) else (
    echo   - No workflow cache found
)

echo.

REM ============================================================================
REM 2. Clear Git reflog (optional - keeps git history clean)
REM ============================================================================
echo [2/4] Clearing Git reflog...

git reflog expire --expire=now --all 2>nul
if %ERRORLEVEL% EQU 0 (
    echo   - Git reflog cleared
) else (
    echo   - Warning: Could not clear git reflog (not in a git repository?)
)

echo.

REM ============================================================================
REM 3. Run Git garbage collection
REM ============================================================================
echo [3/4] Running Git garbage collection...

git gc --prune=now --aggressive 2>nul
if %ERRORLEVEL% EQU 0 (
    echo   - Git garbage collection completed
) else (
    echo   - Warning: Could not run git gc (not in a git repository?)
)

echo.

REM ============================================================================
REM 4. Display instructions for deleting GitHub workflow runs
REM ============================================================================
echo [4/4] Instructions for deleting workflow runs from GitHub:
echo.
echo   Local cleanup complete! To delete workflow runs from GitHub:
echo.
echo   Option 1: Using GitHub Web Interface
echo   -------------------------------------
echo   1. Go to: https://github.com/Conv-AI/Convai-UnrealEngine-SDK-V4/actions
echo   2. Click on a workflow run
echo   3. Click the "..." menu in the top right
echo   4. Select "Delete workflow run"
echo   5. Repeat for each run you want to delete
echo.
echo   Option 2: Using GitHub CLI (gh)
echo   --------------------------------
echo   Install GitHub CLI from: https://cli.github.com/
echo   Then run:
echo     gh run list --limit 100
echo     gh run delete [RUN_ID]
echo.
echo   Option 3: Using PowerShell Script (Bulk Delete)
echo   ------------------------------------------------
echo   Run the companion PowerShell script:
echo     powershell -ExecutionPolicy Bypass -File cleanup_workflow_runs.ps1
echo.

echo ============================================================================
echo   Cleanup Complete!
echo ============================================================================
echo.

pause

