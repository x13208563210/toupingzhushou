const { contextBridge, ipcRenderer } = require('electron')

contextBridge.exposeInMainWorld('liveCastDesktop', {
  getStatus: () => ipcRenderer.invoke('live-cast:get-status'),
  getLogTail: (lines) => ipcRenderer.invoke('live-cast:get-log-tail', lines),
  sendCommand: (action) => ipcRenderer.invoke('live-cast:send-command', action),
  ensureReceiver: () => ipcRenderer.invoke('live-cast:ensure-receiver'),
  openLog: () => ipcRenderer.invoke('live-cast:open-log'),
  openReceiverDir: () => ipcRenderer.invoke('live-cast:open-receiver-dir'),
})
