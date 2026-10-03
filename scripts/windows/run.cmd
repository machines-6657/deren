@echo off
rem ------------------------------------------------------------------------------------------------------------------
rem run.cmd - start the renderer WITHOUT going through the shell's app-reputation check.
rem
rem WHY THIS FILE EXISTS AT ALL, when every other script in this directory is PowerShell: the prompt this avoids is
rem raised by ShellExecuteEx, which is what a DOUBLE-CLICK on an .exe (and PowerShell's `Start-Process`) uses. An
rem .exe carries no signature here - it is built on this machine, for this machine - so Windows asks about the
rem "unknown publisher" on every launch, and a capture batch launches it dozens of times. A .cmd is not an image:
rem the shell runs the Microsoft-signed `cmd.exe`, and the line below starts the renderer with CreateProcess, which
rem has no such check. `scripts\windows\capture.ps1` and `check_render.ps1` were changed the same way (they now use
rem `UseShellExecute = $false`), so the automated runs no longer prompt either.
rem
rem ARGUMENTS ARE FORWARDED, so this is also the way to run a capture by hand:
rem     run.cmd --capture-frames 60 --capture-camera=0,0,2.4,0,-0.9,0
rem The working directory becomes the build directory, which is where the renderer's own relative paths (its
rem `config.toml`, `chars\`, `shaders\`) resolve from.
rem ------------------------------------------------------------------------------------------------------------------
setlocal
set "REPO=%~dp0..\.."
set "BUILD=%REPO%\build-release-clang64"
if not exist "%BUILD%\deren.exe" (
    echo run.cmd: "%BUILD%\deren.exe" does not exist - build it first:
    echo     cmake -S "%REPO%" -B "%BUILD%"
    echo     cmake --build "%BUILD%"
    exit /b 1
)
pushd "%BUILD%"
rem THE CONFIG IS SUPPLIED ONLY IF THE CALLER DID NOT SUPPLY ONE, and that check is not politeness: the renderer
rem takes the FIRST `--config` on its command line, so hardcoding one in front of `%*` silently overrode a config
rem passed by hand - which is how a capture into a scratch directory ended up writing its screenshot into the
rem repository's own folder instead.
echo %* | find /i "--config" >nul
if errorlevel 1 (
    "%BUILD%\deren.exe" --config "%BUILD%\config.toml" %*
) else (
    "%BUILD%\deren.exe" %*
)
set "CODE=%ERRORLEVEL%"
popd
exit /b %CODE%
