const { contextBridge, ipcRenderer } = require('electron');

contextBridge.exposeInMainWorld('bridge', {
  sendCommand: (command) => ipcRenderer.invoke('backend:send', command),
  onLog: (callback) => {
    const sub = (e, val) => callback(val);
    ipcRenderer.on('backend:log', sub);
    return () => ipcRenderer.removeListener('backend:log', sub);
  },
  onEvent: (callback) => {
    const sub = (e, val) => callback(val);
    ipcRenderer.on('backend:event', sub);
    return () => ipcRenderer.removeListener('backend:event', sub);
  }
});
