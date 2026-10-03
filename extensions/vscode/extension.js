// YAPF image preview for VS Code — a read-only custom editor for *.yapf.
// Copyright 2026 Nexoniarz — Apache License 2.0.
const vscode = require('vscode');

class YapfPreviewProvider {
  constructor(context) { this.context = context; }

  openCustomDocument(uri) { return { uri, dispose() {} }; }

  async resolveCustomEditor(document, panel) {
    const media = vscode.Uri.joinPath(this.context.extensionUri, 'media');
    panel.webview.options = { enableScripts: true, localResourceRoots: [media] };
    panel.webview.html = this.html(panel.webview, media);

    const send = async () => {
      try {
        const bytes = await vscode.workspace.fs.readFile(document.uri);
        panel.webview.postMessage({ type: 'file', name: document.uri.path.split('/').pop(), bytes });
      } catch (e) {
        panel.webview.postMessage({ type: 'error', message: String(e.message || e) });
      }
    };
    panel.webview.onDidReceiveMessage((m) => { if (m.type === 'ready') send(); });

    // Reload when the file changes on disk.
    const folder = vscode.Uri.joinPath(document.uri, '..');
    const name = document.uri.path.split('/').pop();
    const watcher = vscode.workspace.createFileSystemWatcher(new vscode.RelativePattern(folder, name));
    watcher.onDidChange(send);
    panel.onDidDispose(() => watcher.dispose());
  }

  html(webview, media) {
    const uri = (f) => webview.asWebviewUri(vscode.Uri.joinPath(media, f));
    const nonce = Math.random().toString(36).slice(2) + Date.now().toString(36);
    return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta http-equiv="Content-Security-Policy"
      content="default-src 'none'; img-src ${webview.cspSource} data: blob:; style-src ${webview.cspSource}; script-src 'nonce-${nonce}';">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<link rel="stylesheet" href="${uri('preview.css')}">
<title>YAPF Preview</title>
</head>
<body>
<div id="bar"><span id="info">Loading…</span><span id="controls"></span></div>
<div id="stage"><canvas id="canvas"></canvas></div>
<div id="error" hidden></div>
<script nonce="${nonce}" src="${uri('yapf.js')}"></script>
<script nonce="${nonce}" src="${uri('preview.js')}"></script>
</body>
</html>`;
  }
}

function activate(context) {
  context.subscriptions.push(vscode.window.registerCustomEditorProvider(
    'yapf.preview', new YapfPreviewProvider(context),
    { webviewOptions: { retainContextWhenHidden: true }, supportsMultipleEditorsPerDocument: true }));
}

function deactivate() {}

module.exports = { activate, deactivate };
