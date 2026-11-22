@echo off
setlocal
pushd "%~dp0" || (
    echo Impossible d'ouvrir le dossier LunaONE.
    pause
    exit /b 1
)

if not exist ".\LunaONE-cli.exe" (
    echo Le fichier LunaONE-cli.exe manque dans ce dossier.
    pause
    popd
    exit /b 1
)

".\LunaONE-cli.exe" --help
if not "%errorlevel%"=="0" echo LunaONE a renvoye le code %errorlevel%.
echo.
echo Pour verifier le demarrage, tape LunaONE-cli.exe --version
"%ComSpec%" /d /k
popd
