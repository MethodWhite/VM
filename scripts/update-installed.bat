@echo off
REM Actualiza la VM instalada (C:\Program Files\VestaVM) con los binarios
REM del build local en cmake-build-release.  Ejecutar como Administrador.

set SRC=F:\C\VM\cmake-build-release
set DST=C:\Program Files\VestaVM

echo Actualizando vesta.exe...
copy /Y "%SRC%\vm.exe" "%DST%\vesta.exe"
if errorlevel 1 goto fail

echo Actualizando plugins nativos...
copy /Y "%SRC%\stdlib\native\io\vesta_io.dll"                   "%DST%\stdlib\native\io\vesta_io.dll"
if errorlevel 1 goto fail
copy /Y "%SRC%\stdlib\native\math\vesta_math.dll"               "%DST%\stdlib\native\math\vesta_math.dll"
if errorlevel 1 goto fail
copy /Y "%SRC%\stdlib\native\collections\vesta_collections.dll" "%DST%\stdlib\native\collections\vesta_collections.dll"
if errorlevel 1 goto fail
copy /Y "%SRC%\stdlib\native\runtime\vex_trace.dll"             "%DST%\stdlib\native\runtime\vex_trace.dll"
if errorlevel 1 goto fail

echo.
echo Actualizacion completa.
exit /b 0

:fail
echo.
echo ERROR: la copia fallo.  Ejecuta este .bat como Administrador.
exit /b 1
