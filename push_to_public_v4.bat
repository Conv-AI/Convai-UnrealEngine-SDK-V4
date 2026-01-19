@echo off
REM ========================================
REM Push Convai folder to remote repo without history
REM Target: https://github.com/Conv-AI/Convai-UnrealEngine-SDK-V4
REM Branch: beta
REM ========================================

setlocal enabledelayedexpansion

echo.
echo ========================================
echo Convai Push Script
echo ========================================
echo.
echo This will push the current Convai folder to:
echo https://github.com/Conv-AI/Convai-UnrealEngine-SDK-V4
echo Branch: beta
echo.
echo WARNING: This will OVERWRITE the beta branch!
echo.
set /p CONFIRM="Are you sure you want to continue? (yes/no): "

if /i not "%CONFIRM%"=="yes" (
    echo.
    echo Operation cancelled.
    pause
    exit /b 0
)

echo.
echo Starting push process...
echo.

REM Create a temporary directory
set TEMP_DIR=%TEMP%\convai_push_%RANDOM%
echo Creating temporary directory: %TEMP_DIR%
mkdir "%TEMP_DIR%"

REM Copy Convai folder contents to temp directory
echo Copying Convai folder to temporary directory...
xcopy /E /I /Y "Convai-UnrealEngine-SDK-Dev\*" "%TEMP_DIR%"

if errorlevel 1 (
    echo ERROR: Failed to copy Convai folder
    rmdir /S /Q "%TEMP_DIR%"
    pause
    exit /b 1
)

REM Navigate to temp directory
cd /d "%TEMP_DIR%"

REM Initialize new git repo
echo Initializing new git repository...
git init -b beta

if errorlevel 1 (
    echo ERROR: Failed to initialize git repository
    cd /d "%~dp0"
    rmdir /S /Q "%TEMP_DIR%"
    pause
    exit /b 1
)

REM Add all files
echo Adding files...
git add .

if errorlevel 1 (
    echo ERROR: Failed to add files
    cd /d "%~dp0"
    rmdir /S /Q "%TEMP_DIR%"
    pause
    exit /b 1
)

REM Commit
echo Creating commit...
git commit -m "Update Convai SDK"

if errorlevel 1 (
    echo ERROR: Failed to commit
    cd /d "%~dp0"
    rmdir /S /Q "%TEMP_DIR%"
    pause
    exit /b 1
)

REM Add remote
echo Adding remote repository...
git remote add origin https://github.com/Conv-AI/Convai-UnrealEngine-SDK-V4.git

if errorlevel 1 (
    echo ERROR: Failed to add remote
    cd /d "%~dp0"
    rmdir /S /Q "%TEMP_DIR%"
    pause
    exit /b 1
)

REM Force push to beta branch
echo.
echo Pushing to beta branch (this may take a while)...
echo.
git push -f origin beta:beta

if errorlevel 1 (
    echo.
    echo ERROR: Failed to push to remote repository
    echo.
    echo This might be due to:
    echo - Authentication issues (you may need to set up a personal access token)
    echo - Network connectivity issues
    echo - Repository access permissions
    echo.
    cd /d "%~dp0"
    rmdir /S /Q "%TEMP_DIR%"
    pause
    exit /b 1
)

REM Clean up
echo.
echo Cleaning up temporary files...
cd /d "%~dp0"
rmdir /S /Q "%TEMP_DIR%"

echo.
echo ========================================
echo SUCCESS!
echo ========================================
echo.
echo The Convai folder has been pushed to:
echo https://github.com/Conv-AI/Convai-UnrealEngine-SDK-V4
echo.
pause

