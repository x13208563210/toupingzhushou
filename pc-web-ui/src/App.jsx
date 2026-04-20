import { useEffect, useMemo, useState } from 'react'
import cnchar from 'cnchar'

const TEXT = {
  waiting: '\u7b49\u5f85\u4e2d',
  preview: '\u9884\u89c8',
  screen: '\u753b\u9762',
  voice: '\u8bed\u97f3',
  danmaku: '\u5f39\u5e55',
  about: '\u8bf4\u660e',
  connected: '\u5df2\u8fde\u63a5',
  disconnected: '\u7b49\u5f85\u8fde\u63a5',
  versionPrefix: '\u7248\u672c ',
  credit: '\u76f4\u64ad\u52a9\u624b\u2014\u2014\u7ca56y',
  adaptiveFps: '\u81ea\u9002\u5e94\u5237\u65b0\u7387',
  waitingSender: '\u7b49\u5f85\u53d1\u9001\u7aef\u8fde\u63a5',
}

const FALLBACK_SNAPSHOT = {
  status: {
    buildLabel: 'web-preview',
    selectedProfile: {
      codec: 'AVC',
      width: 1920,
      height: 1080,
      fps: 60,
      adaptiveFps: true,
    },
    contentFps: 59.8,
    currentLatencyMs: 24.6,
    voiceControlStatus: '\u672a\u5f00\u542f',
    airPlayStatus: '\u5df2\u5b89\u88c5\u672a\u542f\u52a8',
    virtualCameraStatus: '\u5df2\u5b89\u88c5\u672a\u542f\u52a8',
    voiceMusicRootPath: '\u8bed\u97f3\u70b9\u6b4c',
    videoWindowReady: true,
    airPlayRunning: false,
    voiceControlEnabled: false,
    virtualCameraInstalled: true,
    virtualCameraStarting: false,
    virtualCameraRunning: false,
    danmakuRegionReady: false,
    danmakuRunning: false,
    danmakuUiProbeRunning: false,
    danmakuReminderEnabled: true,
    danmakuSpeechEnabled: true,
    danmakuSpeechVoiceCount: 0,
    danmakuSpeechVoiceName: '\u7cfb\u7edf\u9ed8\u8ba4',
    danmakuStatus: '\u7b49\u5f85\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97',
    danmakuRegionLabel: '\u672a\u4f7f\u7528',
    danmakuUiProbeStatus: '\u7b49\u5f85\u63a2\u6d4b\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97',
    danmakuUiProbeTargetTitle: '',
    danmakuLastText: '',
    danmakuCaptureRootPath: '\u5f39\u5e55\u8bc6\u522b',
    danmakuRecentEvents: [],
    danmakuUiProbeLines: [],
  },
}

const LABELS = {
  currentQuality: '\u5f53\u524d\u753b\u8d28',
  fps: '\u5e27\u7387',
  latency: '\u5ef6\u8fdf',
  virtualCamera: '\u865a\u62df\u6444\u50cf\u5934',
  videoWindow: '\u753b\u9762\u7a97\u53e3',
  currentState: '\u5f53\u524d\u72b6\u6001',
  musicFolder: '\u70b9\u6b4c\u76ee\u5f55',
  currentTrack: '\u5f53\u524d\u64ad\u653e',
  voiceModel: '\u8bed\u97f3\u6a21\u578b',
  executing: '\u6267\u884c\u4e2d...',
  openFolder: '\u6253\u5f00\u76ee\u5f55',
  openMusicFolder: '\u6253\u5f00\u70b9\u6b4c\u76ee\u5f55',
  openModelFolder: '\u6253\u5f00\u6a21\u578b\u76ee\u5f55',
  refreshLibrary: '\u5237\u65b0\u66f2\u5e93',
  stopCurrentMusic: '\u505c\u6b62\u70b9\u6b4c',
  openLog: '\u6253\u5f00\u65e5\u5fd7',
  recentVoiceLogs: '\u6700\u8fd1\u8bb0\u5f55',
  recentDanmakuLogs: '\u6700\u8fd1\u8bc6\u522b',
  commandGuide: '\u5e38\u7528\u53e3\u4ee4',
  danmakuState: '\u8bc6\u522b\u72b6\u6001',
  danmakuRegion: '\u60ac\u6d6e\u7a97',
  danmakuUiProbeState: 'UIA\u72b6\u6001',
  danmakuUiProbeTarget: '\u60ac\u6d6e\u7a97',
  danmakuReminder: '\u5f39\u5e55\u63d0\u9192',
  danmakuSpeech: '\u5f39\u5e55\u64ad\u62a5',
  danmakuSpeechVoice: '\u64ad\u62a5\u97f3\u8272',
  danmakuReminderBundle: '\u4e00\u952e\u5f00\u542f\u5f39\u5e55\u63d0\u9192',
  danmakuSpeechBundle: '\u9644\u52a0\u5f00\u542f\u5f39\u5e55\u64ad\u62a5',
  danmakuLastText: '\u6700\u8fd1\u6587\u5b57',
  danmakuCaptureDir: '\u622a\u56fe\u76ee\u5f55',
  probeDanmakuUi: '\u63a2\u6d4b\u4e92\u52a8\u6d88\u606f',
  selectDanmakuRegion: '\u5907\u7528\u9009\u533a',
  testDanmakuFrame: '\u5907\u7528\u6d4b\u8bd5',
  toggleDanmaku: '\u5f00\u59cb\u76d1\u542c',
  stopDanmaku: '\u505c\u6b62\u76d1\u542c',
  toggleDanmakuReminder: '\u65b0\u5f39\u5e55\u63d0\u9192',
  toggleDanmakuSpeech: '\u65b0\u5f39\u5e55\u64ad\u62a5',
  cycleDanmakuSpeechVoice: '\u5207\u6362\u64ad\u62a5\u97f3\u8272',
  openDanmakuFolder: '\u6253\u5f00\u622a\u56fe\u76ee\u5f55',
  clearDanmakuResults: '\u6e05\u7a7a\u7ed3\u679c',
  noDanmakuLogs: '\u6682\u65f6\u8fd8\u6ca1\u6709\u5f39\u5e55\u8bc6\u522b\u7ed3\u679c',
  noUiProbeLogs: '\u6682\u65f6\u8fd8\u6ca1\u6709 UIA \u63a2\u6d4b\u7ed3\u679c',
  addProject: '\u65b0\u589e\u9879\u76ee',
  createProject: '\u521b\u5efa\u9879\u76ee',
  projectName: '\u9879\u76ee\u540d\u79f0',
  projectHint:
    '\u8f93\u5165\u76ee\u5f55\u540d\u79f0\u540e\uff0c\u7a0b\u5e8f\u4f1a\u81ea\u52a8\u521b\u5efa\u76ee\u5f55\uff0c\u5e76\u628a\u540c\u97f3/\u6a21\u7cca\u97f3\u522b\u540d\u76f4\u63a5\u5199\u5165\u522b\u540d.txt\u3002',
  projectPlaceholder: '\u4f8b\u5982\uff1a\u6218\u6b4c\u3001\u7eaf\u97f3\u4e50\u3001\u70ed\u8840',
  projectErrorEmpty: '\u8bf7\u5148\u8f93\u5165\u9879\u76ee\u540d\u79f0',
  ready: '\u5df2\u5c31\u7eea',
  enabled: '\u5df2\u5f00\u542f',
  disabled: '\u5df2\u5173\u95ed',
  waitingFirstFrame: '\u7b49\u5f85\u9996\u5e27',
  idle: '\u7a7a\u95f2',
  noRecentLogs: '\u6682\u65f6\u8fd8\u6ca1\u6709\u8bed\u97f3\u76f8\u5173\u8bb0\u5f55',
}

const MAX_PINYIN_VARIANTS = 24
const MAX_HOMOPHONE_VARIANTS = 48
const MAX_CHAR_CHOICES = 6

const FUZZY_INITIAL_RULES = [
  ['zh', 'z'],
  ['ch', 'c'],
  ['sh', 's'],
  ['z', 'zh'],
  ['c', 'ch'],
  ['s', 'sh'],
  ['l', 'n'],
  ['n', 'l'],
]

const FUZZY_FINAL_RULES = [
  ['ang', 'an'],
  ['an', 'ang'],
  ['eng', 'en'],
  ['en', 'eng'],
  ['ing', 'in'],
  ['in', 'ing'],
  ['iang', 'ian'],
  ['ian', 'iang'],
  ['uang', 'uan'],
  ['uan', 'uang'],
]

const ABOUT_COPY = {
  sectionTitle: '\u7a0b\u5e8f\u4f18\u70b9',
  headline: '\u540c\u4e00\u53f0\u7535\u8111\u5185\u5b8c\u6210\u6295\u5c4f\u63a5\u6536\u3001\u753b\u9762\u63a7\u5236\u548c\u8bed\u97f3\u63a7\u5236',
  description:
    '\u6253\u5f00\u8fd9\u4e00\u4e2a\u7a0b\u5e8f\uff0c\u5c31\u80fd\u5728\u540c\u4e00\u53f0\u7535\u8111\u4e0a\u76f4\u63a5\u5b8c\u6210\u63a5\u6536\u3001\u67e5\u770b\u548c\u5e38\u7528\u64cd\u4f5c\uff0c\u4e0d\u7528\u518d\u5355\u72ec\u5f00\u5176\u4ed6\u63a7\u5236\u7aef\u3002',
  advantagesTitle: '\u4f7f\u7528\u4f18\u70b9',
  advantages: [
    '\u529f\u80fd\u96c6\u4e2d\uff1a\u753b\u9762\u3001\u8bed\u97f3\u3001\u8bf4\u660e\u90fd\u5728\u4e00\u4e2a\u7a97\u53e3\u91cc',
    '\u5f00\u64ad\u65b9\u4fbf\uff1a\u6295\u5c4f\u3001AirPlay\u3001\u865a\u62df\u6444\u50cf\u5934\u90fd\u53ef\u4ee5\u5feb\u901f\u5207\u6362',
    '\u64cd\u4f5c\u76f4\u89c2\uff1a\u5ef6\u8fdf\u3001\u5e27\u7387\u3001\u5f53\u524d\u72b6\u6001\u90fd\u80fd\u76f4\u63a5\u770b\u5230',
  ],
  authorLabel: '\u4f5c\u8005',
  authorName: '\u7ca56y',
}

const DANMAKU_COPY = {
  sectionTitle: '\u5de5\u4f5c\u65b9\u5f0f',
  headline: '\u53ea\u76d1\u542c\u5fae\u4fe1\u89c6\u9891\u53f7\u7684\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97',
  description:
    '\u5f39\u5e55\u9875\u73b0\u5728\u53ea\u8d70 Windows UI Automation \u4e3b\u65b9\u6848\uff0c\u76f4\u63a5\u8bfb\u53d6\u5fae\u4fe1\u89c6\u9891\u53f7\u76f4\u64ad\u4f34\u4fa3\u7684\u201c\u4e92\u52a8\u6d88\u606f\u201d\u60ac\u6d6e\u7a97\u3002',
  tips: [
    '\u5148\u6253\u5f00\u5fae\u4fe1\u89c6\u9891\u53f7\u76f4\u64ad\u4f34\u4fa3\u7684\u201c\u4e92\u52a8\u6d88\u606f\u201d\u60ac\u6d6e\u7a97\u3002',
    '\u70b9\u201c\u4e00\u952e\u5f00\u542f\u5f39\u5e55\u63d0\u9192\u201d\u540e\uff0c\u7a0b\u5e8f\u4f1a\u5148\u81ea\u52a8\u63a2\u6d4b\u201c\u4e92\u52a8\u6d88\u606f\u201d\u60ac\u6d6e\u7a97\uff0c\u518d\u8fdb\u5165\u76d1\u542c\u3002',
    '\u8bc6\u522b\u540e\u4f1a\u76f4\u63a5\u5bf9\u5f53\u524d\u53ef\u89c1\u5217\u8868\u505a\u5dee\u5206\uff0c\u53ea\u6355\u83b7\u65b0\u51fa\u73b0\u7684\u5185\u5bb9\u3002',
    '\u6587\u5b57\u5f39\u5e55\u4f1a\u63d0\u9192\u548c\u64ad\u62a5\uff0c\u975e\u6587\u5b57\u5185\u5bb9\u6309\u793c\u7269\u5904\u7406\u3002',
  ],
}

const VOICE_COMMAND_GUIDE = [
  '\u76f4\u63a5\u8bf4\u9879\u76ee\u540d\uff0c\u547d\u4e2d\u76ee\u5f55\u540e\u4f1a\u5728\u76ee\u5f55\u5185\u968f\u673a\u64ad\u653e',
  '\u65b0\u589e\u9879\u76ee\u540e\uff0c\u4f1a\u81ea\u52a8\u5199\u5165\u522b\u540d.txt\uff0c\u5305\u542b\u540c\u97f3\u5b57\u548c\u6a21\u7cca\u97f3',
  '\u5a92\u4f53\u63a7\u5236\u53ef\u8bf4\uff1a\u201c\u64ad\u653e\u97f3\u4e50\u201d\u3001\u201c\u6682\u505c\u97f3\u4e50\u201d',
  '\u505c\u6b62\u70b9\u6b4c\u53ef\u8bf4\uff1a\u201c\u505c\u6b62\u64ad\u653e\u201d\u3001\u201c\u6709\u4eba\u201d\u3001\u201c\u6765\u4eba\u4e86\u201d',
]

const HERO_COPY = {
  eyebrow: '3D Frosted Console',
  headline: '\u50cf\u62ff\u7740\u624b\u673a\u89d2\u5ea6\u4e00\u6837\u5728\u73bb\u7483\u4e0a\u64cd\u4f5c',
  description:
    '\u628a\u6587\u5b57\u3001\u6309\u94ae\u548c\u72b6\u6001\u4fe1\u606f\u505a\u6210\u5fae\u5fae\u60ac\u6d6e\u5728\u73bb\u7483\u4e0a\u7684\u4e00\u5c42\uff0c\u800c\u4e0d\u662f\u8d34\u5728\u767d\u8272\u80cc\u677f\u91cc\u3002',
  logoText: '\u76f4\u64ad\u6295\u5c4f\u52a9\u624b',
}

function postHostMessage(payload) {
  const webview = window.chrome?.webview
  if (!webview) {
    return false
  }

  webview.postMessage(JSON.stringify(payload))
  return true
}

function addUnique(target, seen, value) {
  const next = `${value ?? ''}`.trim()
  if (!next || seen.has(next)) {
    return
  }
  seen.add(next)
  target.push(next)
}

function isChineseChar(value) {
  return /^[\u3400-\u9fff]$/.test(`${value ?? ''}`)
}

function isAllChineseText(value) {
  return /^[\u3400-\u9fff]+$/.test(`${value ?? ''}`)
}

function buildPinyinSyllableVariants(syllable) {
  const variants = []
  const seen = new Set()
  addUnique(variants, seen, syllable)

  for (const [from, to] of FUZZY_INITIAL_RULES) {
    if (syllable.startsWith(from)) {
      addUnique(variants, seen, `${to}${syllable.slice(from.length)}`)
    }
  }

  for (const [from, to] of FUZZY_FINAL_RULES) {
    if (syllable.endsWith(from)) {
      addUnique(variants, seen, `${syllable.slice(0, syllable.length - from.length)}${to}`)
    }
  }

  const snapshot = [...variants]
  for (const variant of snapshot) {
    for (const [from, to] of FUZZY_INITIAL_RULES) {
      if (variant.startsWith(from)) {
        addUnique(variants, seen, `${to}${variant.slice(from.length)}`)
      }
    }
    for (const [from, to] of FUZZY_FINAL_RULES) {
      if (variant.endsWith(from)) {
        addUnique(variants, seen, `${variant.slice(0, variant.length - from.length)}${to}`)
      }
    }
  }

  return variants
}

function cartesianProductJoin(lists, limit) {
  const result = []

  function walk(index, path) {
    if (result.length >= limit) {
      return
    }
    if (index >= lists.length) {
      result.push(path.join(''))
      return
    }
    for (const item of lists[index]) {
      path.push(item)
      walk(index + 1, path)
      path.pop()
      if (result.length >= limit) {
        return
      }
    }
  }

  walk(0, [])
  return result
}

function buildVoiceProjectAliases(projectName) {
  const trimmed = projectName.trim()
  const aliases = []
  const seen = new Set()
  addUnique(aliases, seen, trimmed)

  if (!trimmed || !isAllChineseText(trimmed)) {
    return aliases
  }

  const chars = Array.from(trimmed)
  const syllables = cnchar
    .spell(trimmed, 'array', 'low')
    .map((item) => `${item || ''}`.toLowerCase().replace(/[^a-z]/g, ''))

  if (syllables.length !== chars.length || syllables.some((item) => !item)) {
    return aliases
  }

  const pinyinVariantLists = syllables.map(buildPinyinSyllableVariants)

  if (chars.length <= 4) {
    const charLists = pinyinVariantLists.map((variants, index) => {
      const values = []
      const valueSeen = new Set()
      addUnique(values, valueSeen, chars[index])
      for (const variant of variants) {
        const words = cnchar.spellToWord(variant, 'alltone', 'array') || []
        for (const word of words) {
          const normalizedWord = `${word}`.trim()
          if (Array.from(normalizedWord).length !== 1 || !isChineseChar(normalizedWord)) {
            continue
          }
          addUnique(values, valueSeen, normalizedWord)
          if (values.length >= MAX_CHAR_CHOICES) {
            break
          }
        }
        if (values.length >= MAX_CHAR_CHOICES) {
          break
        }
      }
      return values
    })

    const homophoneVariants = cartesianProductJoin(charLists, MAX_HOMOPHONE_VARIANTS)
    for (const item of homophoneVariants) {
      addUnique(aliases, seen, item)
    }
  }

  const pinyinVariants = cartesianProductJoin(pinyinVariantLists, MAX_PINYIN_VARIANTS)
  for (const item of pinyinVariants) {
    addUnique(aliases, seen, item)
  }

  return aliases
}

function formatMetric(value, suffix = '') {
  if (value === null || value === undefined || value === '') {
    return TEXT.waiting
  }

  if (typeof value === 'number') {
    return `${value.toFixed(1)}${suffix}`
  }

  return `${value}${suffix}`
}

function buildProfileText(profile) {
  if (!profile) {
    return TEXT.waitingSender
  }

  const fpsText = profile.adaptiveFps ? TEXT.adaptiveFps : `${profile.fps} fps`
  return `${profile.codec} ${profile.width}x${profile.height} / ${fpsText}`
}

function fileNameFromPath(value) {
  const normalized = `${value ?? ''}`.trim().replace(/\\/g, '/')
  if (!normalized) {
    return ''
  }
  const parts = normalized.split('/').filter(Boolean)
  return parts[parts.length - 1] || normalized
}

function buildVoiceActions(status) {
  return [
    {
      key: 'createVoiceMusicProject',
      title: LABELS.addProject,
      detail: '\u81ea\u52a8\u521b\u5efa\u76ee\u5f55\u5e76\u5199\u5165\u522b\u540d.txt',
      buttonText: '\u65b0\u589e',
      modal: true,
      disabled: false,
    },
    {
      action: 'refreshVoiceMusicLibrary',
      title: LABELS.refreshLibrary,
      detail: '\u624b\u52a8\u6dfb\u52a0\u97f3\u4e50\u540e\uff0c\u91cd\u65b0\u7d22\u5f15\u70b9\u6b4c\u76ee\u5f55',
      buttonText: '\u5237\u65b0',
      disabled: false,
    },
    {
      action: 'stopLocalMusicPlayback',
      title: LABELS.stopCurrentMusic,
      detail: status?.localMusicPlaying
        ? `\u6b63\u5728\u64ad\u653e\uff1a${status?.localMusicCurrentName || fileNameFromPath(status?.localMusicCurrentFile)}`
        : '\u5f53\u524d\u6ca1\u6709\u672c\u5730\u70b9\u6b4c\u5728\u64ad\u653e',
      buttonText: status?.localMusicPlaying ? '\u505c\u6b62' : LABELS.idle,
      disabled: false,
    },
    {
      action: 'openVoiceMusicDirectory',
      title: LABELS.openMusicFolder,
      detail: '\u67e5\u770b\u9879\u76ee\u76ee\u5f55\u3001\u522b\u540d.txt \u548c\u97f3\u4e50\u6587\u4ef6',
      buttonText: '\u6253\u5f00',
      disabled: false,
    },
    {
      action: 'openVoiceModelDirectory',
      title: LABELS.openModelFolder,
      detail: '\u67e5\u770b\u6216\u66ff\u6362\u79bb\u7ebf\u8bc6\u522b\u6a21\u578b',
      buttonText: '\u6253\u5f00',
      disabled: false,
    },
    {
      action: 'openLogFile',
      title: LABELS.openLog,
      detail: '\u68c0\u67e5\u8bc6\u522b\u7ed3\u679c\u3001\u70b9\u6b4c\u547d\u4e2d\u548c\u64ad\u653e\u539f\u56e0',
      buttonText: '\u67e5\u770b',
      disabled: false,
    },
  ]
}

function screenActions(status) {
  return [
    {
      action: 'focusVideoWindow',
      title: '\u753b\u9762\u7a97\u53e3',
      detail: status?.videoWindowReady ? '\u805a\u7126\u6295\u5c4f\u753b\u9762' : '\u7b49\u5f85\u9996\u5e27',
      buttonText: '\u6253\u5f00',
      disabled: false,
    },
    {
      action: 'toggleAirPlay',
      title: 'AirPlay',
      detail: status?.airPlayStatus ?? TEXT.waiting,
      buttonText: status?.airPlayRunning ? '\u5173\u95ed' : '\u5f00\u542f',
      disabled: false,
    },
    {
      action: 'toggleVirtualCamera',
      title: '\u865a\u62df\u6444\u50cf\u5934',
      detail: status?.virtualCameraStatus ?? TEXT.waiting,
      buttonText: !status?.virtualCameraInstalled
        ? '\u5b89\u88c5'
        : status?.virtualCameraStarting
          ? '\u542f\u52a8\u4e2d'
          : status?.virtualCameraRunning
            ? '\u5173\u95ed'
            : '\u542f\u52a8',
      disabled: Boolean(status?.virtualCameraStarting),
    },
    {
      action: 'requestKeyframe',
      title: '\u5173\u952e\u5e27\u6062\u590d',
      detail: '\u753b\u9762\u5f02\u5e38\u65f6\u91cd\u62c9\u4e00\u6b21',
      buttonText: '\u8bf7\u6c42',
      disabled: false,
    },
  ]
}

function buildDanmakuActions(status) {
  const reminderEnabled = Boolean(status?.danmakuRunning && status?.danmakuReminderEnabled)
  const speechEnabled = Boolean(
    status?.danmakuRunning && status?.danmakuReminderEnabled && status?.danmakuSpeechEnabled,
  )

  return [
    {
      action: 'toggleDanmakuReminderBundle',
      title: reminderEnabled ? '\u5173\u95ed\u5f39\u5e55\u63d0\u9192' : LABELS.danmakuReminderBundle,
      detail: reminderEnabled
        ? `${status?.danmakuStatus || '\u6b63\u5728\u76d1\u542c\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97'}\uff0c\u6709\u65b0\u5f39\u5e55\u65f6\u4f1a\u64ad\u653e\u63d0\u9192\u97f3`
        : status?.danmakuUiProbeTargetTitle
          ? `\u5c06\u81ea\u52a8\u63a2\u6d4b\u5e76\u76d1\u542c ${status.danmakuUiProbeTargetTitle}\uff0c\u5f00\u542f\u65b0\u5f39\u5e55\u63d0\u9192`
          : '\u70b9\u4e00\u4e0b\u5c31\u4f1a\u81ea\u52a8\u63a2\u6d4b\u60ac\u6d6e\u7a97\uff0c\u5f00\u59cb\u76d1\u542c\u5e76\u64ad\u653e\u63d0\u9192\u97f3',
      buttonText: reminderEnabled ? '\u5173\u95ed' : '\u4e00\u952e\u5f00\u542f',
      disabled: false,
    },
    {
      action: 'toggleDanmakuSpeechBundle',
      title: speechEnabled ? '\u5173\u95ed\u5f39\u5e55\u64ad\u62a5' : LABELS.danmakuSpeechBundle,
      detail: speechEnabled
        ? `\u5f53\u524d\u97f3\u8272\uff1a${status?.danmakuSpeechVoiceName || '\u7cfb\u7edf\u9ed8\u8ba4'}\uff0c\u6587\u5b57\u5f39\u5e55\u4f1a\u6309\u201c\u8c01\u8bf4\u4e86\u4ec0\u4e48\u201d\u64ad\u62a5`
        : `\u5f00\u542f\u65f6\u4f1a\u9644\u5e26\u62c9\u8d77\u5f39\u5e55\u63d0\u9192\uff0c\u5f53\u524d\u97f3\u8272\uff1a${status?.danmakuSpeechVoiceName || '\u7cfb\u7edf\u9ed8\u8ba4'}`,
      buttonText: speechEnabled ? '\u5173\u95ed' : '\u9644\u52a0\u5f00\u542f',
      disabled: false,
    },
  ]
}

function buildDanmakuSummary(status) {
  return [
    {
      label: LABELS.danmakuState,
      value: status?.danmakuStatus || TEXT.waiting,
    },
    {
      label: LABELS.danmakuUiProbeTarget,
      value: status?.danmakuUiProbeTargetTitle || '\u7b49\u5f85\u68c0\u6d4b',
    },
    {
      label: LABELS.danmakuUiProbeState,
      value: status?.danmakuUiProbeStatus || TEXT.waiting,
    },
    {
      label: LABELS.danmakuSpeechVoice,
      value: status?.danmakuSpeechVoiceName || '\u7cfb\u7edf\u9ed8\u8ba4',
      action: 'cycleDanmakuSpeechVoice',
      buttonText: (status?.danmakuSpeechVoiceCount ?? 0) > 1 ? '\u5207\u6362' : '\u68c0\u6d4b',
      disabled: false,
    },
    {
      label: LABELS.danmakuLastText,
      value: status?.danmakuLastText || '\u6682\u65e0',
    },
  ]
}

export default function App() {
  const [page, setPage] = useState('screen')
  const [snapshot, setSnapshot] = useState(FALLBACK_SNAPSHOT)
  const [busyAction, setBusyAction] = useState('')
  const [hostReady, setHostReady] = useState(false)
  const [projectModalOpen, setProjectModalOpen] = useState(false)
  const [projectName, setProjectName] = useState('')
  const [projectError, setProjectError] = useState('')

  useEffect(() => {
    window.__LIVE_CAST_HOST__ = {
      pushSnapshot(nextSnapshot) {
        setSnapshot((previous) => ({
          ...previous,
          ...nextSnapshot,
          status: nextSnapshot?.status ?? previous.status,
        }))
      },
    }

    if (postHostMessage({ type: 'frontend-ready' })) {
      setHostReady(true)
    }

    return () => {
      delete window.__LIVE_CAST_HOST__
    }
  }, [])

  const status = snapshot?.status ?? null
  const voiceRecentLogs = (snapshot?.logs ?? [])
    .filter((line) => /语音|点歌|播放|暂停|停止/.test(`${line ?? ''}`))
    .slice(-6)
    .reverse()
  const buildLabel = status?.buildLabel ?? 'loading'
  const connectionText = hostReady
    ? status?.selectedProfile
      ? TEXT.connected
      : TEXT.disconnected
    : TEXT.preview

  const voiceAction = useMemo(
    () => ({
      action: 'toggleVoiceControl',
      title: status?.voiceControlEnabled ? '\u5173\u95ed\u8bed\u97f3\u63a7\u5236' : '\u5f00\u542f\u8bed\u97f3\u63a7\u5236',
      detail: status?.voiceControlStatus ?? TEXT.waiting,
      buttonText: status?.voiceControlEnabled ? '\u73b0\u5728\u5173\u95ed' : '\u73b0\u5728\u5f00\u542f',
    }),
    [status],
  )

  function runAction(action) {
    setBusyAction(action)
    postHostMessage({ type: 'action', action })
    window.setTimeout(() => setBusyAction(''), 220)
  }

  function handleDragWindow() {
    postHostMessage({ type: 'action', action: 'dragWindow' })
  }

  function openProjectModal() {
    setProjectError('')
    setProjectModalOpen(true)
  }

  function closeProjectModal() {
    setProjectModalOpen(false)
    setProjectError('')
    setProjectName('')
  }

  function submitProjectCreate() {
    const trimmed = projectName.trim()
    if (!trimmed) {
      setProjectError(LABELS.projectErrorEmpty)
      return
    }

    const aliases = buildVoiceProjectAliases(trimmed)
    setBusyAction('createVoiceMusicProject')
    postHostMessage({
      type: 'action',
      action: 'createVoiceMusicProject',
      value: trimmed,
      extra: aliases.join('|'),
    })
    window.setTimeout(() => setBusyAction(''), 260)
    closeProjectModal()
  }

  return (
    <div className="app-shell">
      <div className="butterfly-layer" aria-hidden="true">
        <div className="flashlight" />
        <div className="butterfly">
          <div className="butterfly-glow" />
          <div className="butterfly-body" />
          <div className="butterfly-wing butterfly-wing-left-top" />
          <div className="butterfly-wing butterfly-wing-left-bottom" />
          <div className="butterfly-wing butterfly-wing-right-top" />
          <div className="butterfly-wing butterfly-wing-right-bottom" />
          <div className="butterfly-stream butterfly-stream-1" />
          <div className="butterfly-stream butterfly-stream-2" />
          <div className="butterfly-stream butterfly-stream-3" />
          <div className="butterfly-stream butterfly-stream-4" />
          <div className="butterfly-stream butterfly-stream-5" />
          <div className="butterfly-stream butterfly-stream-6" />
        </div>
        <div className="glow-orbs">
          <div className="glow-orb glow-orb-1" />
          <div className="glow-orb glow-orb-2" />
          <div className="glow-orb glow-orb-3" />
          <div className="glow-orb glow-orb-4" />
          <div className="glow-orb glow-orb-5" />
          <div className="glow-orb glow-orb-6" />
          <div className="glow-orb glow-orb-7" />
        </div>
      </div>
      <main className="app-frame">

        <header className="chrome-row">
          <nav className="tab-row" aria-label="\u9875\u9762\u5207\u6362">
            {[
              ['screen', TEXT.screen],
              ['voice', TEXT.voice],
              ['danmaku', TEXT.danmaku],
              ['about', TEXT.about],
            ].map(([id, label]) => (
              <button
                key={id}
                type="button"
                className={`tab-button ${page === id ? 'is-active' : ''}`}
                onClick={() => setPage(id)}
              >
                {label}
              </button>
            ))}
          </nav>

          <div className="drag-zone" onMouseDown={handleDragWindow} aria-hidden="true" />

          <div className="chrome-actions">
            <span className="status-badge">{connectionText}</span>
            <button
              type="button"
              className="window-button"
              onClick={() => runAction('minimizeWindow')}
              aria-label="\u6700\u5c0f\u5316"
            >
              -
            </button>
            <button
              type="button"
              className="window-button is-danger"
              onClick={() => runAction('closeWindow')}
              aria-label="\u5173\u95ed"
            >
              x
            </button>
          </div>
        </header>

        {page === 'screen' && (
          <section className="page-grid page-grid-screen">
            <section className="metrics-strip">
              <article className="metric-chip metric-chip-wide">
                <span>{LABELS.currentQuality}</span>
                <strong>{buildProfileText(status?.selectedProfile)}</strong>
              </article>
              <article className="metric-chip">
                <span>{LABELS.fps}</span>
                <strong>{formatMetric(status?.contentFps, ' fps')}</strong>
              </article>
              <article className="metric-chip">
                <span>{LABELS.latency}</span>
                <strong>{formatMetric(status?.currentLatencyMs, ' ms')}</strong>
              </article>
            </section>

            <section className="action-grid">
              {screenActions(status).map((item, index) => (
                <button
                  key={item.action}
                  type="button"
                  className={`action-card action-card-${index + 1}`}
                  onClick={() => runAction(item.action)}
                  disabled={busyAction === item.action || item.disabled}
                >
                  <div className="action-card-info">
                    <span>{item.title}</span>
                    <small>{item.detail}</small>
                  </div>
                  <strong>{busyAction === item.action ? LABELS.executing : item.buttonText}</strong>
                </button>
              ))}
            </section>

            <section className="info-row depth-stage is-footer">
              <article className="info-chip info-chip-1">
                <span>AirPlay</span>
                <strong>{status?.airPlayStatus ?? TEXT.waiting}</strong>
              </article>
              <article className="info-chip info-chip-2">
                <span>{LABELS.virtualCamera}</span>
                <strong>{status?.virtualCameraStatus ?? TEXT.waiting}</strong>
              </article>
              <article className="info-chip info-chip-3">
                <span>{LABELS.videoWindow}</span>
                <strong>{status?.videoWindowReady ? LABELS.ready : LABELS.waitingFirstFrame}</strong>
              </article>
            </section>
          </section>
        )}

        {page === 'voice' && (
          <section className="page-grid page-grid-voice">
            <button
              type="button"
              className="voice-toggle"
              onClick={() => runAction(voiceAction.action)}
              disabled={busyAction === voiceAction.action}
            >
              <div className="action-card-info">
                <span>{voiceAction.title}</span>
                <small>{voiceAction.detail}</small>
              </div>
              <strong>{busyAction === voiceAction.action ? LABELS.executing : voiceAction.buttonText}</strong>
            </button>

            <section className="info-row info-row-voice">
              <article className="info-chip">
                <span>{LABELS.currentState}</span>
                <strong>{status?.voiceControlStatus ?? TEXT.waiting}</strong>
              </article>
              <article className="info-chip">
                <span>{LABELS.musicFolder}</span>
                <strong>{status?.voiceMusicRootPath ?? TEXT.waiting}</strong>
              </article>
              <article className="info-chip">
                <span>{LABELS.currentTrack}</span>
                <strong>
                  {status?.localMusicPlaying
                    ? status?.localMusicCurrentName || fileNameFromPath(status?.localMusicCurrentFile)
                    : LABELS.idle}
                </strong>
              </article>
              <article className="info-chip">
                <span>{LABELS.voiceModel}</span>
                <strong>{status?.voiceModelPath ?? TEXT.waiting}</strong>
              </article>
            </section>

            <section className="action-grid voice-action-grid">
              {buildVoiceActions(status).map((item, index) => {
                const actionKey = item.action || item.key
                const isBusy = busyAction === actionKey
                return (
                  <button
                    key={actionKey}
                    type="button"
                    className={`action-card action-card-${index + 1}`}
                    onClick={() => {
                      if (item.modal) {
                        openProjectModal()
                        return
                      }
                      runAction(item.action)
                    }}
                    disabled={isBusy || item.disabled}
                  >
                    <div className="action-card-info">
                      <span>{item.title}</span>
                      <small>{item.detail}</small>
                    </div>
                    <strong>{isBusy ? LABELS.executing : item.buttonText}</strong>
                  </button>
                )
              })}
            </section>

            <section className="voice-bottom-grid">
              <article className="about-card">
                <span className="about-label">{LABELS.commandGuide}</span>
                <strong>\u8fdb\u9875\u9762\u5c31\u80fd\u77e5\u9053\u600e\u4e48\u8bf4</strong>
                <ul className="about-list">
                  {VOICE_COMMAND_GUIDE.map((item) => (
                    <li key={item}>{item}</li>
                  ))}
                </ul>
              </article>

              <article className="about-card">
                <span className="about-label">{LABELS.recentVoiceLogs}</span>
                <strong>\u6700\u8fd1\u7684\u8bc6\u522b\u548c\u70b9\u6b4c\u8bb0\u5f55</strong>
                {voiceRecentLogs.length > 0 ? (
                  <ul className="voice-log-list">
                    {voiceRecentLogs.map((line, index) => (
                      <li key={`${index}-${line}`}>{line}</li>
                    ))}
                  </ul>
                ) : (
                  <p>{LABELS.noRecentLogs}</p>
                )}
              </article>
            </section>
          </section>
        )}

        {page === 'danmaku' && (
          <section className="page-grid page-grid-danmaku">
            <section className="info-row info-row-voice">
              {buildDanmakuSummary(status).map((item) => (
                <article key={item.label} className="info-chip">
                  <span>{item.label}</span>
                  <strong>{item.value}</strong>
                  {item.action && (
                    <button
                      type="button"
                      className="text-button info-chip-button"
                      onClick={() => runAction(item.action)}
                      disabled={busyAction === item.action || item.disabled}
                    >
                      {busyAction === item.action ? LABELS.executing : item.buttonText}
                    </button>
                  )}
                </article>
              ))}
            </section>

            <section className="action-grid danmaku-action-grid">
              {buildDanmakuActions(status).map((item, index) => {
                const isBusy = busyAction === item.action
                return (
                  <button
                    key={item.action}
                    type="button"
                    className={`action-card action-card-${(index % 4) + 1}`}
                    onClick={() => runAction(item.action)}
                    disabled={isBusy || item.disabled}
                  >
                    <div className="action-card-info">
                      <span>{item.title}</span>
                      <small>{item.detail}</small>
                    </div>
                    <strong>{isBusy ? LABELS.executing : item.buttonText}</strong>
                  </button>
                )
              })}
            </section>
          </section>
        )}

        {page === 'about' && (
          <section className="page-grid page-grid-about">
            <section className="about-grid">
              <article className="about-card">
                <span className="about-label">{ABOUT_COPY.sectionTitle}</span>
                <strong>{ABOUT_COPY.headline}</strong>
                <p>{ABOUT_COPY.description}</p>
              </article>

              <article className="about-card">
                <span className="about-label">{ABOUT_COPY.advantagesTitle}</span>
                <ul className="about-list">
                  {ABOUT_COPY.advantages.map((item) => (
                    <li key={item}>{item}</li>
                  ))}
                </ul>
              </article>

              <article className="about-card">
                <span className="about-label">{ABOUT_COPY.authorLabel}</span>
                <strong>{ABOUT_COPY.authorName}</strong>
                <p>QQ 153457128</p>
              </article>
            </section>
          </section>
        )}

        {projectModalOpen && (
          <div className="modal-overlay" role="dialog" aria-modal="true">
            <div className="modal-card">
              <div className="modal-head">
                <strong>{LABELS.addProject}</strong>
                <button type="button" className="window-button" onClick={closeProjectModal} aria-label="关闭">
                  x
                </button>
              </div>
              <label className="modal-field">
                <span>{LABELS.projectName}</span>
                <input
                  className="modal-input"
                  value={projectName}
                  placeholder={LABELS.projectPlaceholder}
                  onChange={(event) => {
                    setProjectName(event.target.value)
                    setProjectError('')
                  }}
                  onKeyDown={(event) => {
                    if (event.key === 'Enter') {
                      submitProjectCreate()
                    }
                    if (event.key === 'Escape') {
                      closeProjectModal()
                    }
                  }}
                  autoFocus
                />
              </label>
              <p className="modal-hint">{LABELS.projectHint}</p>
              {projectError && <p className="modal-error">{projectError}</p>}
              <div className="modal-actions">
                <button type="button" className="text-button" onClick={closeProjectModal}>
                  取消
                </button>
                <button type="button" className="text-button text-button-primary" onClick={submitProjectCreate}>
                  {LABELS.createProject}
                </button>
              </div>
            </div>
          </div>
        )}

        {page !== 'danmaku' && <div className="credit-tag">{TEXT.credit}</div>}
        {page !== 'danmaku' && <div className="build-tag">{`${TEXT.versionPrefix}${buildLabel}`}</div>}
      </main>
    </div>
  )
}
