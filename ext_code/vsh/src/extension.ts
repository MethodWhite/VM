import * as vscode from 'vscode';
import * as path from 'path';
import * as fs from 'fs';

// Terminal reutilizable para ejecutar scripts consecutivos
let _scriptTerminal: vscode.Terminal | undefined;

// Item de la barra de estado (boton de ejecucion rapida)
let _statusBarItem: vscode.StatusBarItem;

// ---------------------------------------------------------------------------
// Activacion de la extension
// ---------------------------------------------------------------------------

export function activate(context: vscode.ExtensionContext): void {
    // Barra de estado: aparece solo cuando hay un .vsh activo
    _statusBarItem = vscode.window.createStatusBarItem(
        vscode.StatusBarAlignment.Right, 100
    );
    _statusBarItem.text       = '$(play) VestaShell';
    _statusBarItem.tooltip    = 'Ejecutar script VestaShell (vsh.runScript)';
    _statusBarItem.command    = 'vsh.runScript';
    _statusBarItem.backgroundColor = new vscode.ThemeColor('statusBarItem.warningBackground');
    context.subscriptions.push(_statusBarItem);

    updateStatusBar();
    context.subscriptions.push(
        vscode.window.onDidChangeActiveTextEditor(updateStatusBar)
    );

    // Limpiar referencia al terminal si el usuario lo cierra manualmente
    context.subscriptions.push(
        vscode.window.onDidCloseTerminal(t => {
            if (t === _scriptTerminal) {
                _scriptTerminal = undefined;
            }
        })
    );

    // Comando: ejecutar el fichero .vsh activo
    context.subscriptions.push(
        vscode.commands.registerCommand('vsh.runScript', async () => {
            const editor = vscode.window.activeTextEditor;
            if (!editor) {
                vscode.window.showWarningMessage('VestaShell: no hay ningun archivo activo.');
                return;
            }
            if (editor.document.languageId !== 'vsh') {
                vscode.window.showWarningMessage('VestaShell: el archivo activo no es un .vsh.');
                return;
            }
            if (editor.document.isDirty) {
                await editor.document.save();
            }
            const filePath = editor.document.fileName;
            const vmPath   = resolveVmPath();
            const terminal = getScriptTerminal();
            terminal.show(true);
            terminal.sendText(buildCmd(vmPath, '--script', filePath));
        })
    );

    // Comando: abrir el REPL interactivo de VestaShell
    context.subscriptions.push(
        vscode.commands.registerCommand('vsh.openRepl', () => {
            const vmPath   = resolveVmPath();
            const terminal = vscode.window.createTerminal({
                name: 'VestaShell REPL',
                message: 'VestaShellScript REPL  (escribe "exit" o "quit" para salir)',
            });
            terminal.show();
            terminal.sendText(buildCmd(vmPath, '--interprete'));
        })
    );

    // Comando: abrir el REPL de VestaVM (modo completo)
    context.subscriptions.push(
        vscode.commands.registerCommand('vsh.openVestaRepl', () => {
            const vmPath   = resolveVmPath();
            const terminal = vscode.window.createTerminal({
                name: 'VestaVM REPL',
                message: 'VestaVM REPL interactivo',
            });
            terminal.show();
            terminal.sendText(buildCmd(vmPath));
        })
    );
}

// ---------------------------------------------------------------------------
// Desactivacion
// ---------------------------------------------------------------------------

export function deactivate(): void {
    _statusBarItem?.dispose();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Muestra u oculta el item de estado segun el lenguaje del editor activo. */
function updateStatusBar(): void {
    const editor = vscode.window.activeTextEditor;
    if (editor && editor.document.languageId === 'vsh') {
        _statusBarItem.show();
    } else {
        _statusBarItem.hide();
    }
}

/**
 * Construye el comando a enviar al terminal segun el SO.
 * En Windows (PowerShell) hay que usar el call operator `&` para ejecutar
 * un path entre comillas; en bash/zsh basta con las comillas directamente.
 */
function buildCmd(exe: string, ...args: string[]): string {
    const quotedExe  = `"${exe}"`;
    const quotedArgs = args.map(a => `"${a}"`).join(' ');
    const suffix     = quotedArgs.length > 0 ? ` ${quotedArgs}` : '';
    return process.platform === 'win32'
        ? `& ${quotedExe}${suffix}`
        : `${quotedExe}${suffix}`;
}

/**
 * Devuelve la ruta al ejecutable vm.
 * Prioridad:
 *   1. Configuracion vsh.vmPath si el usuario la ha definido.
 *   2. build/vesta.exe (Windows) o build/vesta (Linux/Mac) relativo al workspace.
 *   3. "vesta" sin ruta (asume que esta en el PATH del sistema).
 */
function resolveVmPath(): string {
    const cfg     = vscode.workspace.getConfiguration('vsh');
    const cfgPath = cfg.get<string>('vmPath', '');
    if (cfgPath && cfgPath.trim().length > 0) {
        return cfgPath.trim();
    }

    const folders = vscode.workspace.workspaceFolders;
    if (folders && folders.length > 0) {
        const wsRoot   = folders[0].uri.fsPath;
        const isWin    = process.platform === 'win32';
        const candidate = path.join(wsRoot, 'build', isWin ? 'vesta.exe' : 'vesta');
        if (fs.existsSync(candidate)) {
            return candidate;
        }
        // Intentar build2/ como fallback (el proyecto tiene ambas carpetas)
        const candidate2 = path.join(wsRoot, 'build2', isWin ? 'vesta.exe' : 'vesta');
        if (fs.existsSync(candidate2)) {
            return candidate2;
        }
    }
    return 'vesta';
}

/**
 * Devuelve el terminal de ejecucion de scripts.
 * Si vsh.reuseTerminal esta activo y el terminal sigue abierto, lo reutiliza.
 * En caso contrario crea uno nuevo.
 */
function getScriptTerminal(): vscode.Terminal {
    const cfg   = vscode.workspace.getConfiguration('vsh');
    const reuse = cfg.get<boolean>('reuseTerminal', true);
    if (reuse && _scriptTerminal && _scriptTerminal.exitStatus === undefined) {
        return _scriptTerminal;
    }
    _scriptTerminal = vscode.window.createTerminal({
        name: 'VestaShell',
        message: 'Terminal de ejecucion VestaShellScript',
    });
    return _scriptTerminal;
}
