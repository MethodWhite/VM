import * as vscode from 'vscode';
import {
  LanguageClient,
  LanguageClientOptions,
  ServerOptions,
  TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

export function activate(context: vscode.ExtensionContext) {
  const statusBarItem = vscode.window.createStatusBarItem(
    vscode.StatusBarAlignment.Left
  );
  statusBarItem.text = '$(debug-alt) Vex';
  statusBarItem.tooltip = 'Vex Language Status';
  statusBarItem.command = 'vex.build';
  statusBarItem.show();
  context.subscriptions.push(statusBarItem);

  context.subscriptions.push(
    vscode.commands.registerCommand('vex.run', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) return;

      statusBarItem.text = '$(sync~spin) Vex: Running...';
      statusBarItem.backgroundColor = new vscode.ThemeColor(
        'statusBarItem.warningBackground'
      );

      try {
        await runVexFile(editor.document.uri.fsPath, statusBarItem);
        statusBarItem.text = '$(check-all) Vex: Done';
        statusBarItem.backgroundColor = new vscode.ThemeColor(
          'statusBarItem.prominentForeground'
        );
      } catch (err: any) {
        statusBarItem.text = '$(error) Vex: Failed';
        statusBarItem.backgroundColor = new vscode.ThemeColor(
          'statusBarItem.errorBackground'
        );
        vscode.window.showErrorMessage(`Vex run failed: ${err.message}`);
      } finally {
        setTimeout(() => {
          statusBarItem.text = '$(debug-alt) Vex';
        }, 3000);
      }
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand('vex.build', async () => {
      const editor = vscode.window.activeTextEditor;
      if (!editor) return;

      statusBarItem.text = '$(sync~spin) Vex: Building...';
      statusBarItem.backgroundColor = new vscode.ThemeColor(
        'statusBarItem.warningBackground'
      );

      try {
        await buildVexFile(editor.document.uri.fsPath, statusBarItem);
        statusBarItem.text = '$(check-all) Vex: Build OK';
        statusBarItem.backgroundColor = new vscode.ThemeColor(
          'statusBarItem.prominentForeground'
        );
      } catch (err: any) {
        statusBarItem.text = '$(error) Vex: Build Failed';
        statusBarItem.backgroundColor = new vscode.ThemeColor(
          'statusBarItem.errorBackground'
        );
        vscode.window.showErrorMessage(`Vex build failed: ${err.message}`);
      } finally {
        setTimeout(() => {
          statusBarItem.text = '$(debug-alt) Vex';
        }, 3000);
      }
    })
  );

  context.subscriptions.push(
    vscode.commands.registerCommand('vex.test', async () => {
      statusBarItem.text = '$(sync~spin) Vex: Testing...';
      statusBarItem.backgroundColor = new vscode.ThemeColor(
        'statusBarItem.warningBackground'
      );

      try {
        await testVexFiles(statusBarItem);
        statusBarItem.text = '$(check-all) Vex: Tests Passed';
        statusBarItem.backgroundColor = new vscode.ThemeColor(
          'statusBarItem.prominentForeground'
        );
      } catch (err: any) {
        statusBarItem.text = '$(error) Vex: Tests Failed';
        statusBarItem.backgroundColor = new vscode.ThemeColor(
          'statusBarItem.errorBackground'
        );
        vscode.window.showErrorMessage(`Vex tests failed: ${err.message}`);
      } finally {
        setTimeout(() => {
          statusBarItem.text = '$(debug-alt) Vex';
        }, 3000);
      }
    })
  );

  const serverOptions: ServerOptions = {
    command: 'vex-lsp',
    args: [],
    transport: TransportKind.stdio,
  };

  const clientOptions: LanguageClientOptions = {
    documentSelector: [{ scheme: 'file', language: 'vex' }],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.vex'),
    },
  };

  client = new LanguageClient(
    'vex-lsp',
    'Vex Language Server',
    serverOptions,
    clientOptions
  );

  client.start();
}

export function deactivate(): Thenable<void> | undefined {
  if (!client) return undefined;
  return client.stop();
}

async function runVexFile(
  filePath: string,
  _statusBarItem: vscode.StatusBarItem
): Promise<void> {
  const terminal = vscode.window.createTerminal('Vex Run');
  terminal.show();
  terminal.sendText(`vex run "${filePath}"`);
}

async function buildVexFile(
  filePath: string,
  _statusBarItem: vscode.StatusBarItem
): Promise<void> {
  const terminal = vscode.window.createTerminal('Vex Build');
  terminal.show();
  terminal.sendText(`vex build "${filePath}"`);
}

async function testVexFiles(
  _statusBarItem: vscode.StatusBarItem
): Promise<void> {
  const terminal = vscode.window.createTerminal('Vex Test');
  terminal.show();
  terminal.sendText('vex test');
}
