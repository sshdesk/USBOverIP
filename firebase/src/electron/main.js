const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');

let mainWindow = null;
let cppProcess = null;

function resolveBinaryPath() {
  const binaryName = process.platform === 'win32' ? 'gui_usbip.exe' : 'gui_usbip';
  const candidates = [
    path.join(__dirname, '..', 'build', 'Release', binaryName),
    path.join(__dirname, '..', 'build', 'Debug', binaryName),
    path.join(__dirname, '..', 'build', binaryName),
    path.join(__dirname, '..', 'bin', binaryName),
    path.join(__dirname, 'bin', binaryName)
  ];

  for (const p of candidates) {
    if (fs.existsSync(p)) return p;
  }
  return candidates[0];
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 960,
    height: 720,
    minWidth: 800,
    minHeight: 600,
    backgroundColor: '#1e1e24',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false
    }
  });

  mainWindow.loadFile('index.html');
}

function startBackend() {
  const binPath = resolveBinaryPath();
  if (!fs.existsSync(binPath)) {
    console.error(`Binary not found at: ${binPath}`);
    return;
  }

  const workingDir = path.dirname(binPath);
  cppProcess = spawn(binPath, [], {
    cwd: workingDir,
    stdio: ['pipe', 'pipe', 'pipe']
  });

  cppProcess.stdout.on('data', (chunk) => {
    const lines = chunk.toString().split('\n');
    for (const rawLine of lines) {
      const line = rawLine.trim();
      if (!line) continue;

      const eventMatch = line.match(/^IPC_EVENT:\[(.*?)\]\s*(.*)$/);
      if (eventMatch) {
        const type = eventMatch[1];
        const payload = eventMatch[2];
        if (mainWindow && !mainWindow.isDestroyed()) {
          mainWindow.webContents.send('backend:event', { type, payload });
        }
      } else {
        if (mainWindow && !mainWindow.isDestroyed()) {
          mainWindow.webContents.send('backend:log', line);
        }
      }
    }
  });

  cppProcess.stderr.on('data', (chunk) => {
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('backend:log', `[STDERR] ${chunk.toString().trim()}`);
    }
  });

  cppProcess.on('exit', (code, signal) => {
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('backend:event', { type: 'STATE', payload: `Process exited (${code ?? signal})` });
    }
    cppProcess = null;
  });
}

app.whenReady().then(() => {
  createWindow();
  startBackend();
});

app.on('window-all-closed', () => {
  if (cppProcess) {
    try {
      cppProcess.stdin.write('EXIT\n');
    } catch (_) {}
    setTimeout(() => {
      if (cppProcess) cppProcess.kill('SIGTERM');
    }, 1500);
  }
  if (process.platform !== 'darwin') app.quit();
});

ipcMain.handle('backend:send', (event, command) => {
  if (!cppProcess || !cppProcess.stdin.writable) {
    return { success: false, error: 'Backend process is not running.' };
  }
  cppProcess.stdin.write(`${command}\n`);
  return { success: true };
});
