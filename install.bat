@echo off
setlocal

:: --- CONFIGURACIÓN ---
set EXT1=.velb
set EXT2=.vsh
set PROG=%ProgramFiles%\VestaVM\vesta.exe
set ICON=%ProgramFiles%\VestaVM\icono.ico
set CLASS1=VestaVM.velbfile
set CLASS2=VestaVM.vshfile

echo ================================
echo  Instalando asociacion .velb
echo ================================

:: Comprobar permisos de administrador
>nul 2>&1 net session
if %errorlevel% neq 0 (
    echo Debes ejecutar este instalador como administrador.
    pause
    exit /b 1
)

:: Crear la clave de la EXTensión
echo Creando clave HKEY_CLASSES_ROOT\%EXT1%
reg add "HKEY_CLASSES_ROOT\%EXT1%" /ve /d "%CLASS1%" /f

:: Crear la clave de la EXTensión
echo Creando clave HKEY_CLASSES_ROOT\%EXT2%
reg add "HKEY_CLASSES_ROOT\%EXT2%" /ve /d "%CLASS2%" /f

:: Crear la clase del archivo
echo Creando clase %CLASS1%
reg add "HKEY_CLASSES_ROOT\%CLASS1%" /ve /d "Archivo VELB" /f

:: Crear la clase del archivo
echo Creando clase %CLASS2%
reg add "HKEY_CLASSES_ROOT\%CLASS2%" /ve /d "Archivo VSH" /f

:: Asociar icono
echo Asociando icono...
reg add "HKEY_CLASSES_ROOT\%CLASS1%\DefaultIcon" /ve /d "\"%ICON%"\" /f
reg add "HKEY_CLASSES_ROOT\%CLASS2%\DefaultIcon" /ve /d "\"%ICON%"\" /f

:: Crear comando de apertura
echo Creando comando de apertura...
echo "HKEY_CLASSES_ROOT\%CLASS1%\shell\open\command" /ve /d "%PROG% --run %%1" /f
reg add "HKEY_CLASSES_ROOT\%CLASS1%\shell\open\command" /ve /d "\"%PROG%\" --run %%1" /f

echo "HKEY_CLASSES_ROOT\%CLASS2%\shell\open\command" /ve /d "%PROG% --script %%1" /f
reg add "HKEY_CLASSES_ROOT\%CLASS2%\shell\open\command" /ve /d "\"%PROG%\" --script %%1" /f

echo.
echo EXT1ension .velb asociada correctamente.
echo EXT1ension .vsh asociada correctamente.

pause
