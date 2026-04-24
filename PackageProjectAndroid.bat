@echo off
setlocal

REM Set Unreal Engine 5.5 path for the RunUAT.bat script
set UE_PATH=E:\Software\UE_5.6\Engine\Build\BatchFiles\RunUAT.bat

REM Automatically find the .uproject file in the same directory as the script
for %%f in (*.uproject) do (
    set PROJECT_PATH=%%f
    set PROJECT_NAME=%%~nf
)

REM Check if the project was found
if not defined PROJECT_PATH (
    echo No .uproject file found in the current directory.
    pause
    exit /b
)

REM Set configuration to Development and platform to Android
set CONFIG=Development
set PLATFORM=Android

REM Set the output directory to be in the same location as the script, in a "Android" folder.
set SCRIPT_DIR=%~dp0
set TARGETDIR=%SCRIPT_DIR%Android

REM RunUAT.bat to package the project for distribution in Development mode
"%UE_PATH%" BuildCookRun -project=E:/Software/TestProject/TestProject.uproject -noP4 -utf8output -platform=Android -targetplatform=Android -clientconfig=Development -cook -stage -package -compressed -pak -build -prereqs -archive -archivedirectory=E:/Software/TestProject/Android -target=TestProject -ddc=InstalledDerivedDataBackendGraph -installed

echo Done.
pause
