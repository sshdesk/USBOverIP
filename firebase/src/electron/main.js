const { app, BrowserWindow, ipcMain } = require('electron');
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');
const WebSocket = require('ws');

let mainWindow = null;
let cppProcess = null;
let wsClient = null;

// Prevent cache and profile locks when running multiple instances
if (process.env.INSTANCE_ID) {
  app.setPath('userData', path.join(app.getPath('appData'), `webrtc-usbip-gui-${process.env.INSTANCE_ID}`));
}

// Assign distinct ports based on INSTANCE_ID to avoid port conflicts
const backendPort = process.env.INSTANCE_ID === '2' ? 8081 : 8080;

function resolveBinaryPath() {
  const binaryName = process.platform === 'win32' ? 'guiElectron.exe' : 'guiElectron';
  const candidates = [
    path.join(__dirname, '..', 'build-cache-windows_release', 'Release', binaryName),
    path.join(__dirname, '..', 'build-cache-windows_debug', 'Debug', binaryName),
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

function connectWebSocket() {
  wsClient = new WebSocket(`wss://localhost:${backendPort}/ws`, {
    rejectUnauthorized: false
  });

  wsClient.on('open', () => {
    console.log(`Connected to C++ Secure WebSocket backend on port ${backendPort}.`);
  });

  wsClient.on('message', (data) => {
    try {
      const parsed = JSON.parse(data.toString());
      if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.webContents.send('backend:event', parsed);
      }
    } catch (e) {
      if (mainWindow && !mainWindow.isDestroyed()) {
        mainWindow.webContents.send('backend:log', data.toString());
      }
    }
  });

  wsClient.on('close', () => {
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('backend:event', { type: 'STATE', payload: 'Secure WebSocket disconnected. Reconnecting...' });
    }
    setTimeout(connectWebSocket, 2000);
  });

  wsClient.on('error', (err) => {
    console.error('WebSocket error:', err.message);
  });
}

function startBackend() {
  const binPath = resolveBinaryPath();
  if (!fs.existsSync(binPath)) {
    console.error(`Binary not found at: ${binPath}`);
    return;
  }

  const workingDir = path.dirname(binPath);
  // Pass the dynamic port as an argument to the C++ binary
  cppProcess = spawn(binPath, [backendPort.toString()], {
    cwd: workingDir,
    stdio: ['pipe', 'pipe', 'pipe']
  });

  cppProcess.stdout.on('data', (chunk) => {
    const line = chunk.toString().trim();
    if (line && mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('backend:log', line);
    }
  });

  cppProcess.stderr.on('data', (chunk) => {
    const line = chunk.toString().trim();
    if (line && mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('backend:log', `[STDERR] ${line}`);
    }
  });

  cppProcess.on('exit', (code, signal) => {
    if (mainWindow && !mainWindow.isDestroyed()) {
      mainWindow.webContents.send('backend:event', { type: 'STATE', payload: `Process exited (${code ?? signal})` });
    }
    cppProcess = null;
  });

  setTimeout(connectWebSocket, 1000);
}

app.whenReady().then(() => {
  createWindow();
  startBackend();
});

app.on('window-all-closed', () => {
  if (wsClient && wsClient.readyState === WebSocket.OPEN) {
    wsClient.send('EXIT');
    wsClient.close();
  }
  if (cppProcess) {
    try {
      cppProcess.kill('SIGTERM');
    } catch (_) {}
  }
  if (process.platform !== 'darwin') app.quit();
});

ipcMain.handle('backend:send', (event, command) => {
  if (!wsClient || wsClient.readyState !== WebSocket.OPEN) {
    return { success: false, error: 'Secure WebSocket not connected to backend.' };
  }
  wsClient.send(command);
  return { success: true };
});