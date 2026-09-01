@echo off
REM ========================================
REM Push Convai folder to remote repo without history
REM Target: https://github.com/Conv-AI/Convai-UnrealEngine-SDK-V4
REM Branch: beta
REM ========================================

setlocal enabledelayedexpansion
cd /d "%~dp0"

set "SOURCE_DIR=%~dp0."
set "AUTO_CONFIRM=0"
if /i "%~1"=="--yes" set "AUTO_CONFIRM=1"

if not exist "%SOURCE_DIR%\ConvAI.uplugin" (
    echo ERROR: Convai source folder was not found at:
    echo %SOURCE_DIR%
    exit /b 1
)

REM Publish committed files only. This prevents an untracked local file from
REM leaking into the public snapshot and prevents a release from omitting an
REM intended but uncommitted change.
git -C "%SOURCE_DIR%" diff --quiet
if errorlevel 1 (
    echo ERROR: The Convai repository has uncommitted tracked changes.
    echo Commit the release changes before publishing.
    exit /b 1
)
git -C "%SOURCE_DIR%" diff --cached --quiet
if errorlevel 1 (
    echo ERROR: The Convai repository has staged but uncommitted changes.
    echo Commit the release changes before publishing.
    exit /b 1
)

for /f %%A in ('git -C "%SOURCE_DIR%" rev-parse HEAD') do set "SOURCE_SHA=%%A"
for /f "usebackq delims=" %%A in (`powershell -NoProfile -Command "(Get-Content -Raw '%SOURCE_DIR%\ConvAI.uplugin' | ConvertFrom-Json).VersionName"`) do set "PLUGIN_VERSION=%%A"
for /f %%A in ('git ls-remote https://github.com/Conv-AI/Convai-UnrealEngine-SDK-V4.git refs/heads/beta') do set "REMOTE_BETA_SHA=%%A"

echo.
echo ========================================
echo Convai Push Script
echo ========================================
echo.
echo This will push the current Convai folder to:
echo https://github.com/Conv-AI/Convai-UnrealEngine-SDK-V4
echo Branch: beta
echo Version: %PLUGIN_VERSION%
echo Source commit: %SOURCE_SHA%
echo.
echo WARNING: This will OVERWRITE the beta branch!
echo.
if "%AUTO_CONFIRM%"=="1" (
    set "CONFIRM=yes"
) else (
    set /p CONFIRM="Are you sure you want to continue? (yes/no): "
)

if /i not "%CONFIRM%"=="yes" (
    echo.
    echo Operation cancelled.
    if "%AUTO_CONFIRM%"=="0" pause
    exit /b 0
)

echo.
echo Starting push process...
echo.

REM Create a temporary directory
set TEMP_DIR=%TEMP%\convai_push_%RANDOM%
echo Creating temporary directory: %TEMP_DIR%
mkdir "%TEMP_DIR%"

REM Export the committed tree, excluding untracked and ignored local files.
echo Exporting committed Convai files to temporary directory...
git -C "%SOURCE_DIR%" archive --format=zip --output="%TEMP_DIR%\source.zip" HEAD

if errorlevel 1 (
    echo ERROR: Failed to export the Convai repository
    rmdir /S /Q "%TEMP_DIR%"
    if "%AUTO_CONFIRM%"=="0" pause
    exit /b 1
)

tar -xf "%TEMP_DIR%\source.zip" -C "%TEMP_DIR%"
if errorlevel 1 (
    echo ERROR: Failed to extract the committed Convai snapshot
    rmdir /S /Q "%TEMP_DIR%"
    if "%AUTO_CONFIRM%"=="0" pause
    exit /b 1
)
del /Q "%TEMP_DIR%\source.zip"

REM ----------------------------------------
REM Strip items that must NOT go to the public repo.
REM  - Docs: internal maintainer documentation.
REM  - CLAUDE.md and .scratch: internal agent config and issue tracker.
REM  - Content: supplied by the build runner, except source-controlled AgentSkills.
REM  - Source/ThirdParty: supplied by the build runner.
REM  - Source/ConvaiTests and Source/Convai/{Public,Private}/Tests: the test
REM    module and the in-module unit tests. Neither needs anything a customer
REM    has, and CI's marketplace job already drops every Test folder under
REM    Source. Nothing outside those folders includes anything in them.
REM  - Graphify metadata/output: local code-visualization tooling only.
REM ----------------------------------------
echo Removing internal-only items from the publish copy...
if exist "%TEMP_DIR%\.graphifyignore" (
    del /Q "%TEMP_DIR%\.graphifyignore"
    echo - Removed .graphifyignore.
)
if exist "%TEMP_DIR%\graphify-out" (
    rmdir /S /Q "%TEMP_DIR%\graphify-out"
    echo - Removed graphify-out.
)
if exist "%TEMP_DIR%\push_to_public_v4.bat" (
    del /Q "%TEMP_DIR%\push_to_public_v4.bat"
    echo - Removed the developer-only publisher script.
)
if exist "%TEMP_DIR%\CLAUDE.md" (
    del /Q "%TEMP_DIR%\CLAUDE.md"
    echo - Removed CLAUDE.md.
)
if exist "%TEMP_DIR%\.scratch" (
    rmdir /S /Q "%TEMP_DIR%\.scratch"
    echo - Removed .scratch issue tracker.
)
if exist "%TEMP_DIR%\Docs" (
    rmdir /S /Q "%TEMP_DIR%\Docs"
    echo - Removed Docs folder.
) else (
    echo - No Docs folder found, nothing to remove.
)
if exist "%TEMP_DIR%\Content\Skills" (
    move "%TEMP_DIR%\Content\Skills" "%TEMP_DIR%\AgentSkillsPublishOverlay" >nul
)
if exist "%TEMP_DIR%\Content" (
    rmdir /S /Q "%TEMP_DIR%\Content"
    echo - Removed dependency-managed Content.
)
if exist "%TEMP_DIR%\AgentSkillsPublishOverlay" (
    mkdir "%TEMP_DIR%\Content\Skills"
    move "%TEMP_DIR%\AgentSkillsPublishOverlay\*" "%TEMP_DIR%\Content\Skills\" >nul
    rmdir /S /Q "%TEMP_DIR%\AgentSkillsPublishOverlay"
    echo - Restored the source-controlled Content\Skills exception.
)
if exist "%TEMP_DIR%\Source\ThirdParty" (
    rmdir /S /Q "%TEMP_DIR%\Source\ThirdParty"
    echo - Removed dependency-managed Source\ThirdParty.
)
if exist "%TEMP_DIR%\Source\ConvaiTests" (
    rmdir /S /Q "%TEMP_DIR%\Source\ConvaiTests"
    echo - Removed the ConvaiTests module.
)
for %%D in ("%TEMP_DIR%\Source\Convai\Public\Tests" "%TEMP_DIR%\Source\Convai\Private\Tests") do (
    if exist "%%~D" (
        rmdir /S /Q "%%~D"
        echo - Removed the in-module tests: %%~D
    )
)
REM The descriptor has to lose the entry too: a module listed in the .uplugin
REM with no folder on disk fails the customer's packaging, not ours. Edited as
REM JSON rather than by text substitution, the same way VersionName is read.
powershell -NoProfile -Command "$p = '%TEMP_DIR%\ConvAI.uplugin'; $d = Get-Content -Raw $p | ConvertFrom-Json; $d.Modules = @($d.Modules | Where-Object { $_.Name -ne 'ConvaiTests' }); ($d | ConvertTo-Json -Depth 20) | Set-Content -Encoding UTF8 $p"
if errorlevel 1 (
    echo ERROR: Failed to drop the ConvaiTests module from the plugin descriptor.
    rmdir /S /Q "%TEMP_DIR%"
    if "%AUTO_CONFIRM%"=="0" pause
    exit /b 1
)
echo - Dropped the ConvaiTests module from ConvAI.uplugin.


REM Fail closed on the ConvaiTests module, both halves. A descriptor that still
REM lists it fails the customer's packaging; a folder that survives ships the
REM whole framework. F16 is what this is for.
if exist "%TEMP_DIR%\Source\ConvaiTests" (
    echo ERROR: Source\ConvaiTests remained in the public snapshot.
    rmdir /S /Q "%TEMP_DIR%"
    if "%AUTO_CONFIRM%"=="0" pause
    exit /b 1
)
findstr /C:"ConvaiTests" "%TEMP_DIR%\ConvAI.uplugin" >nul 2>&1
if not errorlevel 1 (
    echo ERROR: ConvAI.uplugin still lists the ConvaiTests module.
    rmdir /S /Q "%TEMP_DIR%"
    if "%AUTO_CONFIRM%"=="0" pause
    exit /b 1
)
for %%D in ("%TEMP_DIR%\Source\Convai\Public\Tests" "%TEMP_DIR%\Source\Convai\Private\Tests") do (
    if exist "%%~D" (
        echo ERROR: in-module tests remained in the public snapshot: %%~D
        rmdir /S /Q "%TEMP_DIR%"
        if "%AUTO_CONFIRM%"=="0" pause
        exit /b 1
    )
)

REM Fail closed if Graphify-only files survive under an unexpected path.
for /f "delims=" %%A in ('dir /S /B /A "%TEMP_DIR%\*graphify*" 2^>nul') do (
    echo ERROR: Graphify-only item remained in the public snapshot: %%A
    rmdir /S /Q "%TEMP_DIR%"
    if "%AUTO_CONFIRM%"=="0" pause
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
    if "%AUTO_CONFIRM%"=="0" pause
    exit /b 1
)

REM Add all files
echo Adding files...
git add .

if errorlevel 1 (
    echo ERROR: Failed to add files
    cd /d "%~dp0"
    rmdir /S /Q "%TEMP_DIR%"
    if "%AUTO_CONFIRM%"=="0" pause
    exit /b 1
)

REM Commit
echo Creating commit...
git commit -m "Publish Convai SDK %PLUGIN_VERSION% from %SOURCE_SHA%"

if errorlevel 1 (
    echo ERROR: Failed to commit
    cd /d "%~dp0"
    rmdir /S /Q "%TEMP_DIR%"
    if "%AUTO_CONFIRM%"=="0" pause
    exit /b 1
)

REM Add remote
echo Adding remote repository...
git remote add origin https://github.com/Conv-AI/Convai-UnrealEngine-SDK-V4.git

if errorlevel 1 (
    echo ERROR: Failed to add remote
    cd /d "%~dp0"
    rmdir /S /Q "%TEMP_DIR%"
    if "%AUTO_CONFIRM%"=="0" pause
    exit /b 1
)

REM Force push to beta branch
echo.
echo Pushing to beta branch (this may take a while)...
echo.
git push --force-with-lease=refs/heads/beta:%REMOTE_BETA_SHA% origin beta:beta

if errorlevel 1 goto :push_failed
goto :push_succeeded

:push_failed
REM A transport/client warning can occasionally return a non-zero status after
REM the remote accepted the update. Treat the remote branch as authoritative.
for /f %%A in ('git rev-parse HEAD') do set "LOCAL_PUBLISH_SHA=%%A"
for /f %%A in ('git ls-remote origin refs/heads/beta') do set "PUSHED_BETA_SHA=%%A"
if /i "%PUSHED_BETA_SHA%"=="%LOCAL_PUBLISH_SHA%" (
    echo.
    echo Git reported a warning, but the remote beta branch matches the published commit.
    goto :push_succeeded
)

echo.
echo ERROR: Failed to push to remote repository
echo.
echo This might be due to:
echo - Authentication issues; a personal access token may be required.
echo - Network connectivity issues.
echo - Repository access permissions.
echo.
cd /d "%~dp0"
rmdir /S /Q "%TEMP_DIR%"
if "%AUTO_CONFIRM%"=="0" pause
exit /b 1

:push_succeeded

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
if "%AUTO_CONFIRM%"=="0" pause
