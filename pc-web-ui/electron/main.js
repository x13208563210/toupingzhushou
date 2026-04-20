const { app, BrowserWindow, ipcMain, shell } = require('electron')
const fs = require('fs')
const path = require('path')
const { spawn } = require('child_process')

const isDev = Boolean(process.env.VITE_DEV_SERVER_URL)
const appTitle = '\u76f4\u64ad\u6295\u5c4f\u52a9\u624b\u63a7\u5236\u53f0'
const packagedReceiverFolderName = 'Windows\u63a5\u6536\u7aef'

const appRoot = path.resolve(__dirname, '..')
const repoRoot = path.resolve(appRoot, '..')
const packagedReceiverDir = path.resolve(appRoot, '..', packagedReceiverFolderName)
const devReceiverDir = path.join(repoRoot, 'windows-receiver', 'build-obs-camera', 'Release')
const receiverDir =
  process.env.RECEIVER_DIR || (fs.existsSync(packagedReceiverDir) ? packagedReceiverDir : devReceiverDir)
const statusFile = path.join(receiverDir, 'receiver-status.json')
const logFile = path.join(receiverDir, 'receiver-log.txt')
const commandFile = path.join(receiverDir, 'receiver-command.json')

let mainWindow = null

function resolveReceiverExe() {
  const directExe = path.join(receiverDir, 'AndroidCastReceiver.exe')
  if (fs.existsSync(directExe)) {
    return directExe
  }

  if (!fs.existsSync(receiverDir)) {
    return directExe
  }

  const versionedPrefix = '\u76f4\u64ad\u6295\u5c4f\u52a9\u624b-\u63a5\u6536\u7aef'
  const versioned = fs
    .readdirSync(receiverDir)
    .filter((name) => name.startsWith(versionedPrefix) && name.toLowerCase().endsWith('.exe'))
    .sort()
    .reverse()

  if (versioned.length > 0) {
    return path.join(receiverDir, versioned[0])
  }

  return directExe
}

function ensureReceiverStarted() {
  const receiverExe = resolveReceiverExe()
  if (!fs.existsSync(receiverExe)) {
    return {
      ok: false,
      message: `\u672a\u627e\u5230\u63a5\u6536\u7aef\u6838\u5fc3\uff1a${receiverExe}`,
    }
  }

  const child = spawn(receiverExe, ['--frontend-mode'], {
    cwd: receiverDir,
    detached: true,
    windowsHide: true,
    stdio: 'ignore',
  })
  child.unref()

  return {
    ok: true,
    message: '\u63a5\u6536\u7aef\u6838\u5fc3\u5df2\u5c1d\u8bd5\u5728\u540e\u53f0\u542f\u52a8\u3002',
  }
}

function readJsonFile(filePath) {
  if (!fs.existsSync(filePath)) {
    return null
  }

  try {
    const text = fs.readFileSync(filePath, 'utf8').replace(/^\uFEFF/, '')
    return JSON.parse(text)
  } catch (error) {
    return {
      parseError: String(error),
    }
  }
}

function readLogTail(lines = 120) {
  if (!fs.existsSync(logFile)) {
    return []
  }

  const text = fs.readFileSync(logFile, 'utf8')
  return text
    .split(/\r?\n/)
    .filter(Boolean)
    .slice(-lines)
}

function sendCommand(action) {
  const payload = {
    action,
    requestedAt: Date.now(),
  }
  fs.writeFileSync(commandFile, JSON.stringify(payload, null, 2), 'utf8')
  return {
    ok: true,
  }
}

function pinWindowTitle() {
  if (mainWindow && !mainWindow.isDestroyed()) {
    mainWindow.setTitle(appTitle)
  }
}

function createWindow() {
  mainWindow = new BrowserWindow({
    width: 1500,
    height: 980,
    minWidth: 1120,
    minHeight: 760,
    backgroundColor: '#eef1f6',
    title: appTitle,
    autoHideMenuBar: true,
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: false,
    },
  })

  mainWindow.webContents.on('page-title-updated', (event) => {
    event.preventDefault()
    pinWindowTitle()
  })

  mainWindow.webContents.once('did-finish-load', () => {
    pinWindowTitle()
  })

  if (isDev) {
    mainWindow.loadURL(process.env.VITE_DEV_SERVER_URL)
  } else {
    mainWindow.loadFile(path.join(__dirname, '..', 'dist', 'index.html'))
  }
}

app.whenReady().then(() => {
  ensureReceiverStarted()
  createWindow()

  app.on('activate', () => {
    if (BrowserWindow.getAllWindows().length === 0) {
      createWindow()
    }
  })
})

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit()
  }
})

ipcMain.handle('live-cast:get-status', async () => {
  return {
    receiverDir,
    receiverExe: resolveReceiverExe(),
    status: readJsonFile(statusFile),
  }
})

ipcMain.handle('live-cast:get-log-tail', async (_, lines = 120) => {
  return {
    logPath: logFile,
    lines: readLogTail(lines),
  }
})

ipcMain.handle('live-cast:send-command', async (_, action) => {
  return sendCommand(action)
})

ipcMain.handle('live-cast:ensure-receiver', async () => {
  return ensureReceiverStarted()
})

ipcMain.handle('live-cast:open-log', async () => {
  if (fs.existsSync(logFile)) {
    await shell.openPath(logFile)
  }
  return { ok: true }
})

ipcMain.handle('live-cast:open-receiver-dir', async () => {
  await shell.openPath(receiverDir)
  return { ok: true }
})
