import { useEffect, useMemo, useState } from 'react'
import cnchar from 'cnchar'

const TEXT = {
  waiting: '\u7b49\u5f85\u4e2d',
  preview: '\u9884\u89c8',
  screen: '\u6295\u5c4f\u753b\u9762',
  voice: '\u8bed\u97f3\u63a7\u5236',
  danmaku: '\u5f39\u5e55\u63d0\u9192',
  autoReply: '\u81ea\u52a8\u56de\u590d',
  about: '\u8bf4\u660e',
  connected: '\u5df2\u8fde\u63a5',
  disconnected: '\u7b49\u5f85\u8fde\u63a5',
  versionPrefix: '\u7248\u672c ',
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
    localIpAddress: '192.168.1.88',
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
    danmakuGiftReminderEnabled: true,
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
    autoReplyEnabled: false,
    autoReplySendEnabled: true,
    autoReplyGiftEnabled: false,
    autoReplyScheduleEnabled: false,
    autoReplyCooldownSeconds: 12,
    autoReplyDelayMs: 1600,
    autoReplyScheduleIntervalSeconds: 90,
    autoReplyScheduleNextInSeconds: 0,
    autoReplyPendingCount: 0,
    autoReplyStatus: '\u81ea\u52a8\u56de\u590d\u672a\u5f00\u542f',
    autoReplyConfigPath: '\u5f39\u5e55\u81ea\u52a8\u56de\u590d\\\u81ea\u52a8\u56de\u590d\u914d\u7f6e.json',
    autoReplyTargetTitle: '',
    autoReplyLastMatchText: '',
    autoReplyLastReplyText: '',
    autoReplyInputPointConfigured: false,
    autoReplySendPointConfigured: false,
    autoReplyCaptureRunning: false,
    autoReplyCaptureTarget: '',
    autoReplyInputPointText: '\u672a\u8bb0\u5f55\uff0c\u5f53\u524d\u4f7f\u7528\u9ed8\u8ba4\u8f93\u5165\u6846\u70b9\u4f4d',
    autoReplySendPointText: '\u672a\u8bb0\u5f55\uff0c\u5f53\u524d\u4f7f\u7528\u9ed8\u8ba4\u53d1\u9001\u70b9',
    autoReplyExactRulesText: '',
    autoReplyKeywordRulesText: '',
    autoReplyRulesText: '',
    autoReplyFallbackRepliesText: '',
    autoReplyGiftRepliesText: '',
    autoReplyScheduleRepliesText: '',
    autoReplyRecentLogs: [],
  },
}

const LABELS = {
  currentQuality: '\u5f53\u524d\u753b\u8d28',
  localIpAddress: '\u672c\u673a IP',
  fpsAndLatency: '\u5e27\u7387\u548c\u5ef6\u8fdf',
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
  danmakuRecentEventsTitle: '\u63d0\u9192\u8bb0\u5f55',
  danmakuVisibleTextTitle: '\u60ac\u6d6e\u7a97\u6587\u672c',
  commandGuide: '\u5e38\u7528\u53e3\u4ee4',
  danmakuState: '\u8bc6\u522b\u72b6\u6001',
  danmakuRegion: '\u60ac\u6d6e\u7a97',
  danmakuUiProbeState: '\u8bc6\u522b\u72b6\u6001',
  danmakuUiProbeTarget: '\u60ac\u6d6e\u7a97',
  danmakuReminder: '\u5f39\u5e55\u63d0\u9192',
  danmakuGiftReminder: '\u793c\u7269\u63d0\u9192',
  danmakuSpeech: '\u5f39\u5e55\u64ad\u62a5',
  danmakuSpeechVoice: '\u64ad\u62a5\u97f3\u8272',
  danmakuReminderBundle: '\u4e00\u952e\u5f00\u542f\u5f39\u5e55\u63d0\u9192',
  danmakuGiftReminderBundle: '\u5f00\u542f\u793c\u7269\u63d0\u9192',
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
  noDanmakuEvents: '\u5f00\u542f\u63d0\u9192\u540e\uff0c\u65b0\u5f39\u5e55\u548c\u793c\u7269\u4f1a\u8bb0\u5f55\u5728\u8fd9\u91cc',
  noDanmakuVisibleText: '\u63a2\u6d4b\u5230\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u540e\uff0c\u8fd9\u91cc\u4f1a\u663e\u793a\u5f53\u524d\u53ef\u89c1\u6587\u672c',
  autoReplyMainToggle: '\u81ea\u52a8\u56de\u590d',
  autoReplySendToggle: '\u53d1\u9001\u6d88\u606f',
  autoReplyGiftToggle: '\u793c\u7269\u81ea\u52a8\u611f\u8c22',
  autoReplyScheduleToggle: '\u5b9a\u65f6\u53d1\u9001\u5f39\u5e55',
  autoReplyStatus: '\u5f53\u524d\u72b6\u6001',
  autoReplyTarget: '\u76ee\u6807\u7a97\u53e3',
  autoReplyLastMatch: '\u6700\u8fd1\u547d\u4e2d',
  autoReplyLastReply: '\u6700\u8fd1\u56de\u590d',
  autoReplyInputPoint: '\u8f93\u5165\u6846',
  autoReplySendPoint: '\u53d1\u9001\u6309\u94ae',
  autoReplyRules: '\u89c4\u5219\u914d\u7f6e',
  autoReplyExactRules: '\u7cbe\u786e\u5339\u914d\u56de\u590d',
  autoReplyKeywordRules: '\u5173\u952e\u8bcd\u5339\u914d\u56de\u590d',
  autoReplyFallback: '\u9ed8\u8ba4\u56de\u590d',
  autoReplyGiftReplies: '\u793c\u7269\u611f\u8c22\u8bdd\u672f',
  autoReplyScheduleReplies: '\u5b9a\u65f6\u53d1\u9001\u8bdd\u672f',
  autoReplyTiming: '\u65f6\u95f4\u8bbe\u7f6e',
  autoReplyLogs: '\u6700\u8fd1\u65e5\u5fd7',
  autoReplyFeaturePanelLabel: '\u529f\u80fd\u5f00\u5173',
  autoReplyFeaturePanelTitle: '\u81ea\u52a8\u56de\u590d\u529f\u80fd',
  autoReplyCapturePanelLabel: '\u5750\u6807\u8bb0\u5f55',
  autoReplyCapturePanelTitle: '\u8bb0\u5f55\u5750\u6807',
  autoReplyToolsPanelLabel: '\u5de5\u5177\u64cd\u4f5c',
  autoReplyToolsPanelTitle: '\u5e38\u7528\u64cd\u4f5c',
  autoReplyCooldown: '\u51b7\u5374\u79d2\u6570',
  autoReplyDelay: '\u53d1\u9001\u5ef6\u65f6',
  autoReplyScheduleInterval: '\u5b9a\u65f6\u95f4\u9694',
  autoReplyCaptureInputPoint: '\u8bb0\u5f55\u8f93\u5165\u6846',
  autoReplyCaptureSendPoint: '\u8bb0\u5f55\u53d1\u9001\u6309\u94ae',
  autoReplyReload: '\u91cd\u65b0\u8bfb\u53d6',
  autoReplyOpenFolder: '\u6253\u5f00\u76ee\u5f55',
  autoReplySaveText: '\u4fdd\u5b58\u89c4\u5219',
  autoReplySaveTiming: '\u4fdd\u5b58\u65f6\u95f4',
  autoReplyFillTemplate: '\u586b\u5165\u793a\u4f8b',
  autoReplyRulesHint:
    '\u6bcf\u884c\u4e00\u6761\uff0c\u683c\u5f0f\uff1a\u5173\u952e\u8bcd=\u56de\u590d1|\u56de\u590d2\uff0c\u652f\u6301 *=\u901a\u7528\u56de\u590d\uff0c\u652f\u6301 {\u89c2\u4f17}/{\u5185\u5bb9} \u4e2d\u6587\u5360\u4f4d\u7b26\uff0c\u4e5f\u517c\u5bb9\u65e7\u5199\u6cd5 {name}/{content}\u3002\u5982\u679c\u540c\u65f6\u547d\u4e2d\u591a\u6761\uff0c\u4f1a\u4f18\u5148\u7528\u66f4\u5177\u4f53\u7684\u5173\u952e\u8bcd\u3002',
  autoReplyExactRulesHint:
    '\u6bcf\u884c\u4e00\u6761\uff0c\u683c\u5f0f\uff1a\u5b8c\u6574\u5f39\u5e55=\u56de\u590d1|\u56de\u590d2\u3002\u53ea\u6709\u6574\u53e5\u5b8c\u5168\u4e00\u6837\u65f6\u624d\u4f1a\u56de\u590d\uff0c\u591a\u6761\u5907\u9009\u56de\u590d\u53ea\u7528 | \u5206\u9694\uff0c\u53e5\u5b50\u91cc\u7684\u9017\u53f7\u4f1a\u6309\u6b63\u5e38\u6587\u6848\u4fdd\u7559\u3002\u652f\u6301 {\u89c2\u4f17}/{\u5185\u5bb9} \u4e2d\u6587\u5360\u4f4d\u7b26\uff0c\u4e5f\u517c\u5bb9\u65e7\u5199\u6cd5 {name}/{content}\u3002',
  autoReplyKeywordRulesHint:
    '\u6bcf\u884c\u4e00\u6761\uff0c\u683c\u5f0f\uff1a\u5173\u952e\u8bcd1|\u5173\u952e\u8bcd2=\u56de\u590d1|\u56de\u590d2\u3002\u4e00\u53e5\u8bdd\u91cc\u53ea\u8981\u5305\u542b\u5176\u4e2d\u4efb\u610f\u5173\u952e\u8bcd\u5c31\u4f1a\u89e6\u53d1\uff0c\u957f\u5173\u952e\u8bcd\u4f18\u5148\uff1b\u591a\u6761\u5907\u9009\u56de\u590d\u53ea\u7528 | \u5206\u9694\uff0c\u53e5\u5b50\u91cc\u7684\u9017\u53f7\u4f1a\u6309\u6b63\u5e38\u6587\u6848\u4fdd\u7559\u3002',
  autoReplyFallbackHint:
    '\u6ca1\u6709\u547d\u4e2d\u89c4\u5219\u65f6\uff0c\u4ece\u8fd9\u91cc\u6309\u6362\u884c\u968f\u673a\u62bd\u53d6\u4e00\u6761\u56de\u590d\uff0c\u4e00\u884c\u4e00\u53e5\uff0c\u4e0d\u8981\u7528\u9017\u53f7\u5206\u9694\u3002',
  autoReplyGiftRepliesHint:
    '\u6bcf\u884c\u5199\u4e00\u6761\u611f\u8c22\u8bdd\u672f\uff0c\u6536\u5230\u793c\u7269\u65f6\u4f1a\u6309\u6362\u884c\u968f\u673a\u62bd\u53d6\uff0c\u4e0d\u8981\u7528\u9017\u53f7\u5206\u9694\u3002\u5f53\u8bc6\u522b\u5230\u201c***\u9001\u51fa***\u201d\u65f6\uff0c\u652f\u6301 {\u89c2\u4f17}/{\u793c\u7269}/{\u5185\u5bb9} \u4e2d\u6587\u5360\u4f4d\u7b26\uff0c\u4e5f\u517c\u5bb9\u65e7\u5199\u6cd5 {name}/{speaker}/{gift}/{content}\uff1b\u5982\u679c\u62ff\u4e0d\u5230\u6635\u79f0\uff0c\u7cfb\u7edf\u4f1a\u81ea\u52a8\u6539\u6210\u4e0d\u5e26\u79f0\u547c\u7684\u611f\u8c22\uff0c\u4f8b\u5982\u201c\u611f\u8c22\u9001\u51fa\u7684{\u793c\u7269}\u201d\u3002',
  autoReplyScheduleRepliesHint:
    '\u6bcf\u884c\u5199\u4e00\u6761\u5b9a\u65f6\u5f39\u5e55\uff0c\u5f00\u542f\u540e\u4f1a\u6309\u95f4\u9694\u968f\u673a\u62bd\u53d6\u53d1\u9001\uff0c\u4e00\u884c\u4e00\u53e5\uff0c\u4e0d\u8981\u7528\u9017\u53f7\u5206\u9694\u3002',
  autoReplyPreviewTitle: '\u547d\u4e2d\u9884\u89c8',
  autoReplyPreviewEmpty: '\u8fd8\u6ca1\u6709\u53ef\u751f\u6548\u7684\u5173\u952e\u8bcd\u89c4\u5219\u3002',
  autoReplyPreviewFallbackEmpty: '\u6682\u65f6\u8fd8\u6ca1\u6709\u9ed8\u8ba4\u56de\u590d\u3002',
  autoReplyNoLogs: '\u6682\u65f6\u8fd8\u6ca1\u6709\u81ea\u52a8\u56de\u590d\u65e5\u5fd7',
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
  sectionTitle: '\u7a0b\u5e8f\u8bf4\u660e',
  headline: '\u540c\u4e00\u53f0\u7535\u8111\u5185\u5b8c\u6210\u6295\u5c4f\u63a5\u6536\u3001\u8bed\u97f3\u63a7\u5236\u3001\u5f39\u5e55\u63d0\u9192\u548c\u81ea\u52a8\u56de\u590d',
  description:
    '\u6253\u5f00\u8fd9\u4e00\u4e2a\u7a0b\u5e8f\uff0c\u5c31\u80fd\u5728\u540c\u4e00\u53f0\u7535\u8111\u4e0a\u540c\u65f6\u5b8c\u6210\u753b\u9762\u63a5\u6536\u3001\u8bed\u97f3\u64cd\u4f5c\u3001\u89c2\u5bdf\u5fae\u4fe1\u89c6\u9891\u53f7\u5f39\u5e55\u3001\u4ee5\u53ca\u6309\u89c4\u5219\u81ea\u52a8\u56de\u590d\uff0c\u5f00\u64ad\u65f6\u4e0d\u7528\u5728\u591a\u4e2a\u7a97\u53e3\u4e4b\u95f4\u6765\u56de\u5207\u6362\u3002',
  featuresTitle: '\u9875\u9762\u529f\u80fd',
  features: [
    '\u6295\u5c4f\u753b\u9762\uff1a\u67e5\u770b\u5f53\u524d\u753b\u8d28\u3001\u5e27\u7387\u548c\u5ef6\u8fdf\uff0c\u5feb\u901f\u6253\u5f00\u753b\u9762\u7a97\u53e3\u3001AirPlay\u3001\u865a\u62df\u6444\u50cf\u5934\u3001\u5173\u952e\u5e27\u6062\u590d',
    '\u8bed\u97f3\u63a7\u5236\uff1a\u652f\u6301\u79bb\u7ebf\u8bc6\u522b\u8bed\u97f3\u53e3\u4ee4\uff0c\u70b9\u6b4c\u3001\u64ad\u653e\u3001\u6682\u505c\u3001\u5237\u65b0\u66f2\u5e93\u90fd\u53ef\u4ee5\u76f4\u63a5\u5728\u8fd9\u4e2a\u9875\u9762\u64cd\u4f5c',
    '\u5f39\u5e55\u63d0\u9192\uff1a\u81ea\u52a8\u63a2\u6d4b\u5fae\u4fe1\u89c6\u9891\u53f7\u76f4\u64ad\u52a9\u624b\u7684\u201c\u4e92\u52a8\u6d88\u606f\u201d\u60ac\u6d6e\u7a97\uff0c\u652f\u6301\u65b0\u5f39\u5e55\u63d0\u9192\u3001\u6587\u5b57\u64ad\u62a5\u3001\u793c\u7269\u63d0\u793a',
    '\u81ea\u52a8\u56de\u590d\uff1a\u652f\u6301\u7cbe\u786e\u5339\u914d\u3001\u5173\u952e\u8bcd\u5339\u914d\u3001\u9ed8\u8ba4\u56de\u590d\u3001\u793c\u7269\u81ea\u52a8\u611f\u8c22\u3001\u5b9a\u65f6\u53d1\u9001\u5f39\u5e55\u3001\u51b7\u5374\u65f6\u95f4\u3001\u53d1\u9001\u5ef6\u65f6\u548c\u91cd\u65b0\u8bb0\u5f55\u5750\u6807',
  ],
  workflowTitle: '\u5efa\u8bae\u4f7f\u7528\u6d41\u7a0b',
  workflow: [
    '\u5148\u6253\u5f00\u5fae\u4fe1\u89c6\u9891\u53f7\u76f4\u64ad\u52a9\u624b\uff0c\u5e76\u786e\u8ba4\u201c\u4e92\u52a8\u6d88\u606f\u201d\u60ac\u6d6e\u7a97\u662f\u53ef\u89c1\u72b6\u6001',
    '\u518d\u5230\u201c\u6295\u5c4f\u753b\u9762\u201d\u9875\u786e\u8ba4\u753b\u9762\u548c\u58f0\u97f3\u6b63\u5e38\uff0c\u9700\u8981\u65f6\u53ef\u4ee5\u5f00\u542f AirPlay \u6216\u865a\u62df\u6444\u50cf\u5934',
    '\u5982\u679c\u8981\u505a\u4e92\u52a8\uff0c\u5148\u5728\u201c\u5f39\u5e55\u63d0\u9192\u201d\u9875\u5f00\u542f\u63d0\u9192\u6216\u64ad\u62a5\uff0c\u518d\u5230\u201c\u81ea\u52a8\u56de\u590d\u201d\u9875\u8bbe\u7f6e\u89c4\u5219',
    '\u9996\u6b21\u5728\u65b0\u7535\u8111\u4f7f\u7528\u81ea\u52a8\u56de\u590d\u65f6\uff0c\u9700\u8981\u91cd\u65b0\u8bb0\u5f55\u8f93\u5165\u6846\u548c\u53d1\u9001\u6309\u94ae\u5750\u6807',
  ],
  tipsTitle: '\u6ce8\u610f\u4e8b\u9879',
  tips: [
    '\u5f39\u5e55\u63d0\u9192\u548c\u81ea\u52a8\u56de\u590d\u90fd\u4f9d\u8d56\u5fae\u4fe1\u89c6\u9891\u53f7\u76f4\u64ad\u52a9\u624b\u7684\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97',
    '\u5982\u679c\u66f4\u6362\u7535\u8111\u6216\u8005\u76f4\u64ad\u52a9\u624b\u5e03\u5c40\u53d8\u4e86\uff0c\u8bb0\u5f97\u91cd\u65b0\u8bb0\u5f55\u81ea\u52a8\u56de\u590d\u5750\u6807',
    '\u5982\u679c\u754c\u9762\u6539\u52a8\u540e\u6ca1\u6709\u751f\u6548\uff0c\u8bf7\u786e\u8ba4\u6253\u5f00\u7684\u662f\u6210\u54c1\u76ee\u5f55\u91cc\u7684\u6700\u65b0\u7248\u672c',
  ],
  authorLabel: '\u4f5c\u8005',
  authorName: '\u7ca56y',
  contactLabel: '\u8054\u7cfb\u65b9\u5f0f',
  contactValue: 'QQ 153457128',
}

const AUTO_REPLY_EXACT_RULE_TEMPLATE = [
  '\u4e00\u8d77\u73a9\u5417\uff1f=\u53ef\u4ee5\uff0c\u7a0d\u7b49\u6211\u62c9\u4f60',
  '\u4f60\u597d=\u4f60\u597d\u5440|{\u89c2\u4f17}\u4f60\u597d',
].join('\n')

const AUTO_REPLY_KEYWORD_RULE_TEMPLATE = [
  '\u6b22\u8fce|\u6765\u4e86=\u6b22\u8fce\u6765\u5230\u76f4\u64ad\u95f4|{\u89c2\u4f17}\u665a\u4e0a\u597d',
  '\u600e\u4e48\u4e70|\u4e0b\u5355=\u53ef\u4ee5\u70b9\u5c0f\u9ec4\u8f66\u770b\u770b',
  '\u7ec4\u961f|\u4e00\u8d77\u73a9=\u53ef\u4ee5\uff0c\u7a0d\u7b49\u6211\u62c9\u4f60',
].join('\n')

const AUTO_REPLY_FALLBACK_TEMPLATE = [
  '\u611f\u8c22\u4f60\u7684\u7559\u8a00',
  '\u7a0d\u7b49\u4e00\u4e0b\u6211\u9a6c\u4e0a\u56de\u4f60',
].join('\n')

const AUTO_REPLY_GIFT_TEMPLATE = [
  '\u8c22\u8c22{\u89c2\u4f17}\u9001\u51fa\u7684{\u793c\u7269}',
  '{\u89c2\u4f17}\uff0c\u8fd9\u4e2a{\u793c\u7269}\u6536\u5230\u4e86\uff0c\u771f\u7684\u5f88\u611f\u8c22',
  '\u611f\u8c22{\u89c2\u4f17}\u7684{\u793c\u7269}\u652f\u6301',
].join('\n')

const AUTO_REPLY_SCHEDULE_TEMPLATE = [
  '\u6b22\u8fce\u65b0\u6765\u7684\u670b\u53cb\u70b9\u70b9\u5173\u6ce8',
  '\u559c\u6b22\u4e3b\u64ad\u7684\u53ef\u4ee5\u70b9\u4e2a\u8d5e',
  '\u60f3\u4ea4\u6d41\u7684\u53ef\u4ee5\u76f4\u63a5\u53d1\u5f39\u5e55',
].join('\n')

const VOICE_COMMAND_GUIDE = [
  '\u76f4\u63a5\u8bf4\u9879\u76ee\u540d\uff0c\u547d\u4e2d\u76ee\u5f55\u540e\u4f1a\u5728\u76ee\u5f55\u5185\u968f\u673a\u64ad\u653e',
  '\u65b0\u589e\u9879\u76ee\u540e\uff0c\u4f1a\u81ea\u52a8\u5199\u5165\u522b\u540d.txt\uff0c\u5305\u542b\u540c\u97f3\u5b57\u548c\u6a21\u7cca\u97f3',
  '\u5a92\u4f53\u63a7\u5236\u53ef\u8bf4\uff1a\u201c\u64ad\u653e\u97f3\u4e50\u201d\u3001\u201c\u6682\u505c\u97f3\u4e50\u201d',
  '\u505c\u6b62\u70b9\u6b4c\u53ef\u8bf4\uff1a\u201c\u505c\u6b62\u64ad\u653e\u201d\u3001\u201c\u6709\u4eba\u201d\u3001\u201c\u6765\u4eba\u4e86\u201d',
]

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

function splitAutoReplyLines(text) {
  return `${text ?? ''}`
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
}

function splitAutoReplyItems(text) {
  return `${text ?? ''}`
    .split(/[|,，/、;；]/)
    .map((item) => item.trim())
    .filter(Boolean)
}

function parseAutoReplyReplyPool(text) {
  const replies = []
  splitAutoReplyLines(text).forEach((line) => {
    splitAutoReplyItems(line).forEach((item) => replies.push(item))
  })
  return replies
}

function parseAutoReplyRulesPreview(text) {
  const rules = []
  const invalidLines = []

  splitAutoReplyLines(text).forEach((line) => {
    if (line.startsWith('#') || line.startsWith('//')) {
      return
    }

    const asciiPos = line.indexOf('=')
    const fullwidthPos = line.indexOf('＝')
    const splitPos = asciiPos >= 0 ? asciiPos : fullwidthPos
    if (splitPos < 0) {
      invalidLines.push(line)
      return
    }

    const label = line.slice(0, splitPos).trim()
    const replies = parseAutoReplyReplyPool(line.slice(splitPos + 1))
    if (!label || replies.length === 0) {
      invalidLines.push(line)
      return
    }

    const wildcard = label === '*'
    const keywords = wildcard ? ['*'] : splitAutoReplyItems(label)
    if (!keywords.length) {
      invalidLines.push(line)
      return
    }

    rules.push({
      label,
      keywords,
      replies,
      wildcard,
    })
  })

  return {
    rules,
    invalidLines,
  }
}

function buildAutoReplyPreviewMeta(preview, unitLabel) {
  const base = `\u5df2\u8bc6\u522b ${preview.rules.length} \u6761${unitLabel}`
  return preview.invalidLines.length > 0
    ? `${base}\uff0c\u53e6\u6709 ${preview.invalidLines.length} \u884c\u683c\u5f0f\u4e0d\u5b8c\u6574`
    : base
}

function splitAutoReplyItemsSafe(text) {
  return `${text ?? ''}`
    .split(/[|,\/\uFF0C\u3001;\uFF1B\uFF5C]/)
    .map((item) => item.trim())
    .filter(Boolean)
}

function parseAutoReplyReplyPoolSafe(text) {
  return splitAutoReplyLines(text)
}

function splitAutoReplyRuleReplyItemsSafe(text) {
  return `${text ?? ''}`
    .split(/[|\uFF5C]/)
    .map((item) => item.trim())
    .filter(Boolean)
}

function parseAutoReplyRuleReplyPoolSafe(text) {
  const replies = []
  splitAutoReplyLines(text).forEach((line) => {
    splitAutoReplyRuleReplyItemsSafe(line).forEach((item) => replies.push(item))
  })
  return replies
}

function parseAutoReplyRulesPreviewSafe(text) {
  const rules = []
  const invalidLines = []

  splitAutoReplyLines(text).forEach((line) => {
    if (line.startsWith('#') || line.startsWith('//')) {
      return
    }

    const asciiPos = line.indexOf('=')
    const fullwidthPos = line.indexOf('\uFF1D')
    const splitPos = asciiPos >= 0 ? asciiPos : fullwidthPos
    if (splitPos < 0) {
      invalidLines.push(line)
      return
    }

    const label = line.slice(0, splitPos).trim()
    const replies = parseAutoReplyRuleReplyPoolSafe(line.slice(splitPos + 1))
    if (!label || replies.length === 0) {
      invalidLines.push(line)
      return
    }

    const wildcard = label === '*'
    const keywords = wildcard ? ['*'] : splitAutoReplyItemsSafe(label)
    if (!keywords.length) {
      invalidLines.push(line)
      return
    }

    rules.push({
      label,
      keywords,
      replies,
      wildcard,
    })
  })

  return {
    rules,
    invalidLines,
  }
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

function buildLocalIpText(value) {
  const normalized = `${value ?? ''}`.trim()
  return normalized || '\u6682\u672a\u83b7\u53d6'
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
  const giftReminderEnabled = Boolean(
    status?.danmakuRunning && status?.danmakuGiftReminderEnabled,
  )
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
      action: 'toggleDanmakuGiftReminderBundle',
      title: giftReminderEnabled ? '\u5173\u95ed\u793c\u7269\u63d0\u9192' : LABELS.danmakuGiftReminderBundle,
      detail: giftReminderEnabled
        ? '\u76d1\u542c\u4e92\u52a8\u6d88\u606f\u4e0a\u65b9\u7684\u9001\u793c\u533a\uff0c\u6709\u65b0\u793c\u7269\u65f6\u4f1a\u64ad\u653e\u63d0\u9192\u97f3'
        : status?.danmakuUiProbeTargetTitle
          ? `\u5c06\u7ee7\u7eed\u76d1\u542c ${status.danmakuUiProbeTargetTitle}\uff0c\u5355\u72ec\u4e3a\u65b0\u793c\u7269\u6d88\u606f\u64ad\u653e\u63d0\u9192\u97f3`
          : '\u5f00\u542f\u540e\u4f1a\u81ea\u52a8\u76d1\u542c\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\uff0c\u4e13\u95e8\u63d0\u9192\u65b0\u793c\u7269',
      buttonText: giftReminderEnabled ? '\u5173\u95ed' : '\u5f00\u542f',
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
      variant: 'main',
      details: [
        `${LABELS.danmakuReminder}：${status?.danmakuReminderEnabled ? LABELS.enabled : LABELS.disabled}`,
        `${LABELS.danmakuGiftReminder}：${status?.danmakuGiftReminderEnabled ? LABELS.enabled : LABELS.disabled}`,
        `${LABELS.danmakuSpeech}：${status?.danmakuSpeechEnabled ? LABELS.enabled : LABELS.disabled}`,
      ],
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
      variant: 'full',
    },
  ]
}

function buildAutoReplySummary(status) {
  return [
    {
      label: LABELS.autoReplyLastMatch,
      value: status?.autoReplyLastMatchText || '\u6682\u65e0',
    },
    {
      label: LABELS.autoReplyLastReply,
      value: status?.autoReplyLastReplyText || '\u6682\u65e0',
    },
  ]
}

function buildAutoReplyActions(status) {
  const captureTarget = `${status?.autoReplyCaptureTarget || ''}`
  const scheduleInterval = status?.autoReplyScheduleIntervalSeconds ?? 90
  const scheduleNextIn = status?.autoReplyScheduleNextInSeconds ?? 0

  return [
    {
      action: 'toggleAutoReplyEnabled',
      compactTitle: '\u81ea\u52a8\u56de\u590d',
      enabled: Boolean(status?.autoReplyEnabled),
      title: LABELS.autoReplyMainToggle,
      detail: status?.autoReplyEnabled
        ? '\u5df2\u5f00\u542f\u89c4\u5219\u5339\u914d\uff0c\u4f1a\u76d1\u542c\u65b0\u5f39\u5e55'
        : '\u5f00\u542f\u540e\u81ea\u52a8\u63a2\u6d4b\u4e92\u52a8\u6d88\u606f\u5e76\u5f00\u59cb\u5339\u914d\u89c4\u5219',
      buttonText: status?.autoReplyEnabled ? '\u5173\u95ed' : '\u5f00\u542f',
    },
    {
      action: 'toggleAutoReplyGiftEnabled',
      compactTitle: '\u793c\u7269\u56de\u590d',
      enabled: Boolean(status?.autoReplyGiftEnabled),
      title: LABELS.autoReplyGiftToggle,
      detail: status?.autoReplyGiftEnabled
        ? '\u6536\u5230\u65b0\u793c\u7269\u540e\uff0c\u4f1a\u5957\u7528\u4e0b\u65b9\u7684\u611f\u8c22\u8bdd\u672f'
        : '\u5f00\u542f\u540e\uff0c\u65b0\u793c\u7269\u4f1a\u5355\u72ec\u8d70\u81ea\u52a8\u611f\u8c22',
      buttonText: status?.autoReplyGiftEnabled ? '\u5173\u95ed' : '\u5f00\u542f',
    },
    {
      action: 'toggleAutoReplyScheduleEnabled',
      compactTitle: '\u5b9a\u65f6\u53d1\u9001',
      enabled: Boolean(status?.autoReplyScheduleEnabled),
      title: LABELS.autoReplyScheduleToggle,
      detail: status?.autoReplyScheduleEnabled
        ? !status?.autoReplyEnabled
          ? '\u5df2\u914d\u7f6e\uff0c\u91cd\u65b0\u6253\u5f00\u81ea\u52a8\u56de\u590d\u540e\u751f\u6548'
          : `\u6bcf ${scheduleInterval} \u79d2\u89e6\u53d1\u4e00\u6b21${scheduleNextIn > 0 ? `\uff0c\u7ea6 ${scheduleNextIn} \u79d2\u540e\u5f00\u59cb` : ''}`
        : '\u5f00\u542f\u540e\u4f1a\u6309\u95f4\u9694\u4ece\u4e0b\u65b9\u8bdd\u672f\u91cc\u968f\u673a\u53d1\u9001',
      buttonText: status?.autoReplyScheduleEnabled ? '\u5173\u95ed' : '\u5f00\u542f',
    },
    {
      action: 'captureAutoReplyInputPoint',
      title: LABELS.autoReplyCaptureInputPoint,
      detail:
        status?.autoReplyCaptureRunning && captureTarget === 'input'
          ? '\u73b0\u5728\u53bb\u70b9\u4e00\u4e0b\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u5e95\u90e8\u201c\u6dfb\u52a0\u8bc4\u8bba...\u201d\u8f93\u5165\u6846'
          : status?.autoReplyInputPointText || '\u672a\u8bb0\u5f55\uff0c\u5f53\u524d\u4f7f\u7528\u9ed8\u8ba4\u8f93\u5165\u6846\u70b9\u4f4d',
      buttonText:
        status?.autoReplyCaptureRunning && captureTarget === 'input'
          ? '\u7b49\u5f85\u70b9\u51fb'
          : status?.autoReplyInputPointConfigured
            ? '\u91cd\u65b0\u8bb0\u5f55'
            : '\u5f00\u59cb\u8bb0\u5f55',
    },
    {
      action: 'captureAutoReplySendPoint',
      title: LABELS.autoReplyCaptureSendPoint,
      detail:
        status?.autoReplyCaptureRunning && captureTarget === 'send'
          ? '\u73b0\u5728\u53bb\u70b9\u4e00\u4e0b\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97\u53f3\u4e0b\u89d2\u201c\u53d1\u9001\u201d'
          : status?.autoReplySendPointText || '\u672a\u8bb0\u5f55\uff0c\u5f53\u524d\u4f7f\u7528\u9ed8\u8ba4\u53d1\u9001\u6309\u94ae\u70b9\u4f4d',
      buttonText:
        status?.autoReplyCaptureRunning && captureTarget === 'send'
          ? '\u7b49\u5f85\u70b9\u51fb'
          : status?.autoReplySendPointConfigured
            ? '\u91cd\u65b0\u8bb0\u5f55'
            : '\u5f00\u59cb\u8bb0\u5f55',
    },
    {
      action: 'reloadAutoReplyConfig',
      title: LABELS.autoReplyReload,
      detail: '\u91cd\u65b0\u4ece\u672c\u5730\u914d\u7f6e\u6587\u4ef6\u8bfb\u53d6\u89c4\u5219\u4e0e\u65f6\u95f4\u8bbe\u7f6e',
      buttonText: '\u5237\u65b0',
    },
    {
      action: 'openAutoReplyDirectory',
      title: LABELS.autoReplyOpenFolder,
      detail: status?.autoReplyConfigPath || '\u6253\u5f00\u81ea\u52a8\u56de\u590d\u76ee\u5f55',
      buttonText: '\u6253\u5f00',
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
  const [autoReplyExactRules, setAutoReplyExactRules] = useState('')
  const [autoReplyKeywordRules, setAutoReplyKeywordRules] = useState('')
  const [autoReplyFallback, setAutoReplyFallback] = useState('')
  const [autoReplyGiftReplies, setAutoReplyGiftReplies] = useState('')
  const [autoReplyScheduleReplies, setAutoReplyScheduleReplies] = useState('')
  const [autoReplyCooldown, setAutoReplyCooldown] = useState('12')
  const [autoReplyDelay, setAutoReplyDelay] = useState('1600')
  const [autoReplyScheduleInterval, setAutoReplyScheduleInterval] = useState('90')
  const [autoReplyTextDirty, setAutoReplyTextDirty] = useState(false)
  const [autoReplyTimingDirty, setAutoReplyTimingDirty] = useState(false)

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
  const danmakuRecentEvents = status?.danmakuRecentEvents ?? []
  const danmakuVisibleText = status?.danmakuUiProbeLines ?? []
  const danmakuSummaryItems = buildDanmakuSummary(status)
  const autoReplyRecentLogs = [...(status?.autoReplyRecentLogs ?? [])].reverse()
  const autoReplyStatusText = status?.autoReplyStatus || TEXT.waiting
  const autoReplySummaryItems = buildAutoReplySummary(status)
  const autoReplyActionItems = buildAutoReplyActions(status)
  const autoReplyToggleItems = autoReplyActionItems.slice(0, 3)
  const autoReplyManageItems = autoReplyActionItems.slice(3)
  const autoReplyCaptureItems = autoReplyManageItems.filter(
    (item) => item.action === 'captureAutoReplyInputPoint' || item.action === 'captureAutoReplySendPoint',
  )
  const autoReplyExactRulePreview = useMemo(
    () => parseAutoReplyRulesPreviewSafe(autoReplyExactRules),
    [autoReplyExactRules],
  )
  const autoReplyKeywordRulePreview = useMemo(
    () => parseAutoReplyRulesPreviewSafe(autoReplyKeywordRules),
    [autoReplyKeywordRules],
  )
  const autoReplyFallbackPreview = useMemo(
    () => parseAutoReplyReplyPoolSafe(autoReplyFallback),
    [autoReplyFallback],
  )
  const autoReplyGiftPreview = useMemo(
    () => parseAutoReplyReplyPoolSafe(autoReplyGiftReplies),
    [autoReplyGiftReplies],
  )
  const autoReplySchedulePreview = useMemo(
    () => parseAutoReplyReplyPoolSafe(autoReplyScheduleReplies),
    [autoReplyScheduleReplies],
  )
  const resolvedVoiceRecentLogs = useMemo(
    () =>
      (snapshot?.logs ?? [])
        .filter((line) => /\u8bed\u97f3|\u70b9\u6b4c|\u64ad\u653e|\u6682\u505c|\u505c\u6b62/.test(`${line ?? ''}`))
        .slice(-6)
        .reverse(),
    [snapshot?.logs],
  )

  useEffect(() => {
    if (!autoReplyTextDirty) {
      setAutoReplyExactRules(status?.autoReplyExactRulesText ?? '')
      setAutoReplyKeywordRules(status?.autoReplyKeywordRulesText ?? status?.autoReplyRulesText ?? '')
      setAutoReplyFallback(status?.autoReplyFallbackRepliesText ?? '')
      setAutoReplyGiftReplies(status?.autoReplyGiftRepliesText ?? '')
      setAutoReplyScheduleReplies(status?.autoReplyScheduleRepliesText ?? '')
    }
  }, [
    status?.autoReplyExactRulesText,
    status?.autoReplyKeywordRulesText,
    status?.autoReplyRulesText,
    status?.autoReplyFallbackRepliesText,
    status?.autoReplyGiftRepliesText,
    status?.autoReplyScheduleRepliesText,
    autoReplyTextDirty,
  ])

  useEffect(() => {
    if (!autoReplyTimingDirty) {
      setAutoReplyCooldown(`${status?.autoReplyCooldownSeconds ?? 12}`)
      setAutoReplyDelay(`${status?.autoReplyDelayMs ?? 1600}`)
      setAutoReplyScheduleInterval(`${status?.autoReplyScheduleIntervalSeconds ?? 90}`)
    }
  }, [
    status?.autoReplyCooldownSeconds,
    status?.autoReplyDelayMs,
    status?.autoReplyScheduleIntervalSeconds,
    autoReplyTimingDirty,
  ])

  const voiceAction = useMemo(
    () => ({
      action: 'toggleVoiceControl',
      title: status?.voiceControlEnabled ? '\u5173\u95ed\u8bed\u97f3\u63a7\u5236' : '\u5f00\u542f\u8bed\u97f3\u63a7\u5236',
      detail: status?.voiceControlStatus ?? TEXT.waiting,
      buttonText: status?.voiceControlEnabled ? '\u73b0\u5728\u5173\u95ed' : '\u73b0\u5728\u5f00\u542f',
    }),
    [status],
  )

  function runAction(action, value = '', extra = '') {
    setBusyAction(action)
    postHostMessage({ type: 'action', action, value, extra })
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

  function saveAutoReplyText() {
    setAutoReplyTextDirty(false)
    runAction(
      'saveAutoReplyTextConfig',
      JSON.stringify({
        exactRulesText: autoReplyExactRules,
        keywordRulesText: autoReplyKeywordRules,
        rulesText: autoReplyKeywordRules,
        fallbackRepliesText: autoReplyFallback,
        giftRepliesText: autoReplyGiftReplies,
        scheduleRepliesText: autoReplyScheduleReplies,
      }),
    )
  }

  function saveAutoReplyTiming() {
    setAutoReplyTimingDirty(false)
    runAction('saveAutoReplyTiming', `${autoReplyCooldown}|${autoReplyDelay}|${autoReplyScheduleInterval}`)
  }

  function fillAutoReplyTemplate() {
    setAutoReplyExactRules(AUTO_REPLY_EXACT_RULE_TEMPLATE)
    setAutoReplyKeywordRules(AUTO_REPLY_KEYWORD_RULE_TEMPLATE)
    setAutoReplyFallback(AUTO_REPLY_FALLBACK_TEMPLATE)
    setAutoReplyGiftReplies(AUTO_REPLY_GIFT_TEMPLATE)
    setAutoReplyScheduleReplies(AUTO_REPLY_SCHEDULE_TEMPLATE)
    setAutoReplyTextDirty(true)
  }

  function reloadAutoReplyConfig() {
    setAutoReplyTextDirty(false)
    setAutoReplyTimingDirty(false)
    runAction('reloadAutoReplyConfig')
  }

  return (
    <div className="app-shell">
      <main className="app-frame">
        <header className="chrome-row">
          <nav className="tab-row" aria-label="\u9875\u9762\u5207\u6362">
            {[
              ['screen', TEXT.screen],
              ['voice', TEXT.voice],
              ['danmaku', TEXT.danmaku],
              ['auto-reply', TEXT.autoReply],
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

        <div className="frame-body">
          {page === 'screen' && (
            <section className="page-grid page-grid-screen">
              <section className="metrics-strip">
                <article className="metric-chip metric-chip-wide">
                  <span>{LABELS.currentQuality}</span>
                  <strong>{buildProfileText(status?.selectedProfile)}</strong>
                  <small className="metric-chip-subtext">
                    {LABELS.localIpAddress}\uff1a{buildLocalIpText(status?.localIpAddress)}
                  </small>
                </article>
                <article className="metric-chip metric-chip-combined">
                  <span>{LABELS.fpsAndLatency}</span>
                  <div className="metric-pair">
                    <div className="metric-pair-item">
                      <small>{LABELS.fps}</small>
                      <strong>{formatMetric(status?.contentFps, ' fps')}</strong>
                    </div>
                    <div className="metric-pair-item">
                      <small>{LABELS.latency}</small>
                      <strong>{formatMetric(status?.currentLatencyMs, ' ms')}</strong>
                    </div>
                  </div>
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
                  {resolvedVoiceRecentLogs.length > 0 ? (
                    <ul className="voice-log-list">
                      {resolvedVoiceRecentLogs.map((line, index) => (
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
              <section className="info-row danmaku-summary-grid">
                {danmakuSummaryItems.map((item) => (
                  <article
                    key={item.label}
                    className={`info-chip danmaku-summary-item${item.variant ? ` is-${item.variant}` : ''}`}
                  >
                    <span>{item.label}</span>
                    <strong>{item.value}</strong>
                    {item.details?.length > 0 && (
                      <div className="danmaku-summary-details">
                        {item.details.map((detail) => (
                          <small key={detail}>{detail}</small>
                        ))}
                      </div>
                    )}
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

              <section className="danmaku-bottom-grid">
                <article className="about-card danmaku-log-card">
                  <span className="about-label">{LABELS.danmakuRecentEventsTitle}</span>
                  <strong>{LABELS.recentDanmakuLogs}</strong>
                  {danmakuRecentEvents.length > 0 ? (
                    <ul className="voice-log-list danmaku-log-list">
                      {danmakuRecentEvents.map((line, index) => (
                        <li key={`${index}-${line}`}>{line}</li>
                      ))}
                    </ul>
                  ) : (
                    <p>{LABELS.noDanmakuEvents}</p>
                  )}
                </article>

                <article className="about-card danmaku-log-card">
                  <span className="about-label">{LABELS.danmakuVisibleTextTitle}</span>
                  <strong>{status?.danmakuUiProbeTargetTitle || '\u7b49\u5f85\u63a2\u6d4b\u4e92\u52a8\u6d88\u606f\u60ac\u6d6e\u7a97'}</strong>
                  {danmakuVisibleText.length > 0 ? (
                    <ul className="voice-log-list danmaku-log-list">
                      {danmakuVisibleText.map((line, index) => (
                        <li key={`${index}-${line}`}>{line}</li>
                      ))}
                    </ul>
                  ) : (
                    <p>{LABELS.noDanmakuVisibleText}</p>
                  )}
                </article>
              </section>
            </section>
          )}

          {page === 'about' && (
            <section className="page-grid page-grid-about">
              <section className="about-grid">
                <article className="about-card about-card-hero">
                  <span className="about-label">{ABOUT_COPY.sectionTitle}</span>
                  <strong>{ABOUT_COPY.headline}</strong>
                  <p>{ABOUT_COPY.description}</p>
                </article>

                <article className="about-card">
                  <span className="about-label">{ABOUT_COPY.featuresTitle}</span>
                  <ul className="about-list">
                    {ABOUT_COPY.features.map((item) => (
                      <li key={item}>{item}</li>
                    ))}
                  </ul>
                </article>

                <article className="about-card">
                  <span className="about-label">{ABOUT_COPY.workflowTitle}</span>
                  <ul className="about-list">
                    {ABOUT_COPY.workflow.map((item) => (
                      <li key={item}>{item}</li>
                    ))}
                  </ul>
                </article>

                <article className="about-card">
                  <span className="about-label">{ABOUT_COPY.tipsTitle}</span>
                  <ul className="about-list">
                    {ABOUT_COPY.tips.map((item) => (
                      <li key={item}>{item}</li>
                    ))}
                  </ul>
                </article>

                <article className="about-card">
                  <span className="about-label">{ABOUT_COPY.authorLabel}</span>
                  <strong>{ABOUT_COPY.authorName}</strong>
                  <p>{`${ABOUT_COPY.contactLabel}：${ABOUT_COPY.contactValue}`}</p>
                </article>
              </section>
            </section>
          )}

          {page === 'auto-reply' && (
            <section className="page-grid page-grid-auto-reply">
              <section className="auto-reply-hero-grid">
                <article className="about-card auto-reply-toggle-panel">
                  <div className="auto-reply-toggle-top">
                    <div className="auto-reply-toggle-head">
                      <span className="about-label">{LABELS.autoReplyFeaturePanelLabel}</span>
                      <strong>{LABELS.autoReplyFeaturePanelTitle}</strong>
                      <p>{`\u53d1\u9001\u9ed8\u8ba4\u5f00\u542f\uff0c\u961f\u5217 ${status?.autoReplyPendingCount ?? 0} \u6761`}</p>
                    </div>
                    <div className="auto-reply-status-embed">
                      <span>{LABELS.autoReplyStatus}</span>
                      <strong>{autoReplyStatusText}</strong>
                    </div>
                  </div>
                  <div className="auto-reply-toggle-grid">
                    {autoReplyToggleItems.map((item) => {
                      const isBusy = busyAction === item.action
                      return (
                        <button
                          key={item.action}
                          type="button"
                          className={`auto-reply-toggle-button${item.enabled ? ' is-on' : ''}${isBusy ? ' is-busy' : ''}`}
                          onClick={() => runAction(item.action)}
                          title={item.detail}
                          aria-pressed={item.enabled}
                          disabled={isBusy}
                        >
                          <div className="auto-reply-toggle-copy compact">
                            <span>{item.compactTitle || item.title}</span>
                          </div>
                          <div className="auto-reply-toggle-switch-wrap" aria-hidden="true">
                            <span className={`auto-reply-toggle-switch${item.enabled ? ' is-on' : ''}`}>
                              <span className="auto-reply-toggle-switch-thumb" />
                            </span>
                          </div>
                        </button>
                      )
                    })}
                  </div>
                </article>
              </section>

              <section className="info-row auto-reply-summary-row">
                {autoReplySummaryItems.map((item) => (
                  <article key={item.label} className="info-chip auto-reply-summary-chip">
                    <span>{item.label}</span>
                    <strong>{item.value}</strong>
                  </article>
                ))}
              </section>

              <section className="action-grid auto-reply-action-grid">
                {autoReplyCaptureItems.length > 0 && (
                  <article className="about-card auto-reply-capture-card auto-reply-capture-card-full">
                    <div className="auto-reply-capture-head auto-reply-capture-head-row">
                      <div className="auto-reply-capture-head-copy">
                        <span className="about-label">{LABELS.autoReplyCapturePanelLabel}</span>
                        <strong>{LABELS.autoReplyCapturePanelTitle}</strong>
                      </div>
                      <button
                        type="button"
                        className="text-button info-chip-button auto-reply-capture-inline-button"
                        onClick={reloadAutoReplyConfig}
                        disabled={busyAction === 'reloadAutoReplyConfig'}
                      >
                        {busyAction === 'reloadAutoReplyConfig' ? LABELS.executing : LABELS.autoReplyReload}
                      </button>
                    </div>
                    <div className="auto-reply-capture-list">
                      {autoReplyCaptureItems.map((item) => {
                        const isBusy = busyAction === item.action
                        return (
                          <button
                            key={item.action}
                            type="button"
                            className="auto-reply-capture-button"
                            onClick={() => runAction(item.action)}
                            disabled={isBusy}
                          >
                            <div className="auto-reply-capture-copy">
                              <span>{item.title}</span>
                              <small>{item.detail}</small>
                            </div>
                            <strong>{isBusy ? LABELS.executing : item.buttonText}</strong>
                          </button>
                        )
                      })}
                    </div>
                  </article>
                )}
              </section>

              <section className="auto-reply-editor-grid">
                <article className="about-card auto-reply-card">
                <span className="about-label">{LABELS.autoReplyExactRules}</span>
                <strong>{LABELS.autoReplyExactRules}</strong>
                <p>{LABELS.autoReplyExactRulesHint}</p>
                <textarea
                  className="auto-reply-textarea"
                  value={autoReplyExactRules}
                  placeholder={
                    '\u4f8b\u5982\uff1a\n\u4e00\u8d77\u73a9\u5417\uff1f=\u53ef\u4ee5\uff0c\u7a0d\u7b49\u6211\u62c9\u4f60\n\u4f60\u597d=\u4f60\u597d\u5440|{\u89c2\u4f17}\u4f60\u597d'
                  }
                  onChange={(event) => {
                    setAutoReplyExactRules(event.target.value)
                    setAutoReplyTextDirty(true)
                  }}
                />
                <div className="auto-reply-card-actions">
                  <button type="button" className="text-button" onClick={fillAutoReplyTemplate}>
                    {LABELS.autoReplyFillTemplate}
                  </button>
                </div>
                <div className="auto-reply-preview-block">
                  <span className="about-label">{LABELS.autoReplyPreviewTitle}</span>
                  <p className="auto-reply-preview-meta">
                    {buildAutoReplyPreviewMeta(autoReplyExactRulePreview, '\u7cbe\u786e\u89c4\u5219')}
                  </p>
                  {autoReplyExactRulePreview.rules.length > 0 ? (
                    <ul className="voice-log-list auto-reply-preview-list">
                      {autoReplyExactRulePreview.rules.slice(0, 5).map((rule, index) => (
                        <li key={`${rule.label}-${index}`}>
                          {`${rule.wildcard ? '\u901a\u7528\u89c4\u5219' : rule.keywords.join(' / ')} -> ${rule.replies
                            .slice(0, 2)
                            .join(' / ')}`}
                          {rule.replies.length > 2 ? ` \u7b49 ${rule.replies.length} \u6761\u56de\u590d` : ''}
                        </li>
                      ))}
                    </ul>
                  ) : (
                    <p className="auto-reply-preview-empty">{LABELS.autoReplyPreviewEmpty}</p>
                  )}
                </div>
              </article>

              <article className="about-card auto-reply-card">
                <span className="about-label">{LABELS.autoReplyKeywordRules}</span>
                <strong>{LABELS.autoReplyKeywordRules}</strong>
                <p>{LABELS.autoReplyKeywordRulesHint}</p>
                <textarea
                  className="auto-reply-textarea"
                  value={autoReplyKeywordRules}
                  placeholder={
                    '\u4f8b\u5982\uff1a\n\u6b22\u8fce|\u6765\u4e86=\u6b22\u8fce\u6765\u5230\u76f4\u64ad\u95f4|{\u89c2\u4f17}\u665a\u4e0a\u597d\n\u600e\u4e48\u4e70|\u4e0b\u5355=\u53ef\u4ee5\u70b9\u5c0f\u9ec4\u8f66\u770b\u770b\n*=\u611f\u8c22\u652f\u6301'
                  }
                  onChange={(event) => {
                    setAutoReplyKeywordRules(event.target.value)
                    setAutoReplyTextDirty(true)
                  }}
                />
                <div className="auto-reply-card-actions">
                  <button
                    type="button"
                    className="text-button text-button-primary"
                    onClick={saveAutoReplyText}
                    disabled={busyAction === 'saveAutoReplyTextConfig'}
                  >
                    {busyAction === 'saveAutoReplyTextConfig' ? LABELS.executing : LABELS.autoReplySaveText}
                  </button>
                </div>
                <div className="auto-reply-preview-block">
                  <span className="about-label">{LABELS.autoReplyPreviewTitle}</span>
                  <p className="auto-reply-preview-meta">
                    {buildAutoReplyPreviewMeta(autoReplyKeywordRulePreview, '\u5173\u952e\u8bcd\u89c4\u5219')}
                  </p>
                  {autoReplyKeywordRulePreview.rules.length > 0 ? (
                    <ul className="voice-log-list auto-reply-preview-list">
                      {autoReplyKeywordRulePreview.rules.slice(0, 5).map((rule, index) => (
                        <li key={`${rule.label}-${index}`}>
                          {`${rule.wildcard ? '\u901a\u7528\u89c4\u5219' : rule.keywords.join(' / ')} -> ${rule.replies
                            .slice(0, 2)
                            .join(' / ')}`}
                          {rule.replies.length > 2 ? ` \u7b49 ${rule.replies.length} \u6761\u56de\u590d` : ''}
                        </li>
                      ))}
                    </ul>
                  ) : (
                    <p className="auto-reply-preview-empty">{LABELS.autoReplyPreviewEmpty}</p>
                  )}
                </div>
              </article>

              <article className="about-card auto-reply-card">
                <span className="about-label">{LABELS.autoReplyGiftReplies}</span>
                <strong>{LABELS.autoReplyGiftReplies}</strong>
                <p>{LABELS.autoReplyGiftRepliesHint}</p>
                <textarea
                  className="auto-reply-textarea"
                  value={autoReplyGiftReplies}
                  placeholder={'\u8c22\u8c22{\u89c2\u4f17}\u9001\u51fa\u7684{\u793c\u7269}\n{\u89c2\u4f17}\uff0c\u8fd9\u4e2a{\u793c\u7269}\u6536\u5230\u4e86'}
                  onChange={(event) => {
                    setAutoReplyGiftReplies(event.target.value)
                    setAutoReplyTextDirty(true)
                  }}
                />
                <div className="auto-reply-preview-block">
                  <span className="about-label">{LABELS.autoReplyGiftReplies}</span>
                  <p className="auto-reply-preview-meta">
                    {`\u5f53\u524d\u6709 ${autoReplyGiftPreview.length} \u6761\u793c\u7269\u611f\u8c22\u8bdd\u672f`}
                  </p>
                  {autoReplyGiftPreview.length > 0 ? (
                    <ul className="voice-log-list auto-reply-preview-list">
                      {autoReplyGiftPreview.slice(0, 5).map((reply, index) => (
                        <li key={`${reply}-${index}`}>{reply}</li>
                      ))}
                    </ul>
                  ) : (
                    <p className="auto-reply-preview-empty">{LABELS.autoReplyPreviewFallbackEmpty}</p>
                  )}
                </div>
              </article>

              <article className="about-card auto-reply-card">
                <span className="about-label">{LABELS.autoReplyFallback}</span>
                <strong>{LABELS.autoReplyFallback}</strong>
                <p>{LABELS.autoReplyFallbackHint}</p>
                <textarea
                  className="auto-reply-textarea"
                  value={autoReplyFallback}
                  placeholder={'\u611f\u8c22\u4f60\u7684\u7559\u8a00\n\u7a0d\u7b49\u4e00\u4e0b\u6211\u9a6c\u4e0a\u56de\u4f60'}
                  onChange={(event) => {
                    setAutoReplyFallback(event.target.value)
                    setAutoReplyTextDirty(true)
                  }}
                />
                <div className="auto-reply-preview-block">
                  <span className="about-label">{LABELS.autoReplyFallback}</span>
                  <p className="auto-reply-preview-meta">
                    {`\u5f53\u524d\u6709 ${autoReplyFallbackPreview.length} \u6761\u9ed8\u8ba4\u56de\u590d`}
                  </p>
                  {autoReplyFallbackPreview.length > 0 ? (
                    <ul className="voice-log-list auto-reply-preview-list">
                      {autoReplyFallbackPreview.slice(0, 5).map((reply, index) => (
                        <li key={`${reply}-${index}`}>{reply}</li>
                      ))}
                    </ul>
                  ) : (
                    <p className="auto-reply-preview-empty">{LABELS.autoReplyPreviewFallbackEmpty}</p>
                  )}
                </div>
              </article>

              <article className="about-card auto-reply-card auto-reply-card-wide">
                <span className="about-label">{LABELS.autoReplyScheduleReplies}</span>
                <strong>{LABELS.autoReplyScheduleReplies}</strong>
                <p>{LABELS.autoReplyScheduleRepliesHint}</p>
                <textarea
                  className="auto-reply-textarea"
                  value={autoReplyScheduleReplies}
                  placeholder={'\u6b22\u8fce\u65b0\u6765\u7684\u670b\u53cb\u70b9\u70b9\u5173\u6ce8\n\u559c\u6b22\u4e3b\u64ad\u7684\u53ef\u4ee5\u70b9\u4e2a\u8d5e'}
                  onChange={(event) => {
                    setAutoReplyScheduleReplies(event.target.value)
                    setAutoReplyTextDirty(true)
                  }}
                />
                <div className="auto-reply-preview-block">
                  <span className="about-label">{LABELS.autoReplyScheduleReplies}</span>
                  <p className="auto-reply-preview-meta">
                    {`\u5f53\u524d\u6709 ${autoReplySchedulePreview.length} \u6761\u5b9a\u65f6\u5f39\u5e55\u8bdd\u672f`}
                  </p>
                  {autoReplySchedulePreview.length > 0 ? (
                    <ul className="voice-log-list auto-reply-preview-list">
                      {autoReplySchedulePreview.slice(0, 6).map((reply, index) => (
                        <li key={`${reply}-${index}`}>{reply}</li>
                      ))}
                    </ul>
                  ) : (
                    <p className="auto-reply-preview-empty">{LABELS.autoReplyPreviewFallbackEmpty}</p>
                  )}
                </div>
              </article>
            </section>

            {false && (
            <section className="auto-reply-editor-grid">
              <article className="about-card auto-reply-card">
                <span className="about-label">{LABELS.autoReplyRules}</span>
                <strong>{AUTO_REPLY_COPY.sectionTitle}</strong>
                <p>{LABELS.autoReplyRulesHint}</p>
                <textarea
                  className="auto-reply-textarea"
                  value={autoReplyRules}
                  placeholder={'\u4f8b\u5982\uff1a\n\u6b22\u8fce=\u6b22\u8fce\u6765\u5230\u76f4\u64ad\u95f4|{name}\u665a\u4e0a\u597d\n\u600e\u4e48\u4e70|\u4e0b\u5355=\u53ef\u4ee5\u70b9\u5c0f\u9ec4\u8f66\u770b\u770b\n*=\u611f\u8c22\u652f\u6301'}
                  onChange={(event) => {
                    setAutoReplyRules(event.target.value)
                    setAutoReplyTextDirty(true)
                  }}
                />
                <div className="auto-reply-card-actions">
                  <button
                    type="button"
                    className="text-button"
                    onClick={fillAutoReplyTemplate}
                  >
                    {LABELS.autoReplyFillTemplate}
                  </button>
                  <button
                    type="button"
                    className="text-button text-button-primary"
                    onClick={saveAutoReplyText}
                    disabled={busyAction === 'saveAutoReplyTextConfig'}
                  >
                    {busyAction === 'saveAutoReplyTextConfig' ? LABELS.executing : LABELS.autoReplySaveText}
                  </button>
                </div>
                <div className="auto-reply-preview-block">
                  <span className="about-label">{LABELS.autoReplyPreviewTitle}</span>
                  <p className="auto-reply-preview-meta">
                    {`已识别 ${autoReplyRulePreview.rules.length} 条关键词规则`}
                    {autoReplyRulePreview.invalidLines.length > 0
                      ? `，另有 ${autoReplyRulePreview.invalidLines.length} 行格式不完整`
                      : ''}
                  </p>
                  {autoReplyRulePreview.rules.length > 0 ? (
                    <ul className="voice-log-list auto-reply-preview-list">
                      {autoReplyRulePreview.rules.slice(0, 5).map((rule, index) => (
                        <li key={`${rule.label}-${index}`}>
                          {`${rule.wildcard ? '通用规则' : rule.keywords.join(' / ')} -> ${rule.replies
                            .slice(0, 2)
                            .join(' / ')}`}
                          {rule.replies.length > 2 ? ` 等 ${rule.replies.length} 条回复` : ''}
                        </li>
                      ))}
                    </ul>
                  ) : (
                    <p className="auto-reply-preview-empty">{LABELS.autoReplyPreviewEmpty}</p>
                  )}
                </div>
              </article>

              <article className="about-card auto-reply-card">
                <span className="about-label">{LABELS.autoReplyFallback}</span>
                <strong>{LABELS.autoReplyFallback}</strong>
                <p>{LABELS.autoReplyFallbackHint}</p>
                <textarea
                  className="auto-reply-textarea"
                  value={autoReplyFallback}
                  placeholder={'\u611f\u8c22\u4f60\u7684\u7559\u8a00\n\u7a0d\u7b49\u4e00\u4e0b\u6211\u9a6c\u4e0a\u56de\u4f60'}
                  onChange={(event) => {
                    setAutoReplyFallback(event.target.value)
                    setAutoReplyTextDirty(true)
                  }}
                />
                <div className="auto-reply-preview-block">
                  <span className="about-label">{LABELS.autoReplyFallback}</span>
                  <p className="auto-reply-preview-meta">{`当前有 ${autoReplyFallbackPreview.length} 条兜底回复`}</p>
                  {autoReplyFallbackPreview.length > 0 ? (
                    <ul className="voice-log-list auto-reply-preview-list">
                      {autoReplyFallbackPreview.slice(0, 5).map((reply, index) => (
                        <li key={`${reply}-${index}`}>{reply}</li>
                      ))}
                    </ul>
                  ) : (
                    <p className="auto-reply-preview-empty">{LABELS.autoReplyPreviewFallbackEmpty}</p>
                  )}
                </div>
              </article>
            </section>

            )}

            <section className="auto-reply-bottom-grid">
              <article className="about-card auto-reply-card">
                <span className="about-label">{LABELS.autoReplyTiming}</span>
                <strong>{LABELS.autoReplyTiming}</strong>
                <div className="auto-reply-form-grid">
                  <label className="auto-reply-field">
                    <span>{LABELS.autoReplyCooldown}</span>
                    <input
                      className="auto-reply-input"
                      value={autoReplyCooldown}
                      onChange={(event) => {
                        setAutoReplyCooldown(event.target.value.replace(/[^\d]/g, ''))
                        setAutoReplyTimingDirty(true)
                      }}
                    />
                  </label>
                  <label className="auto-reply-field">
                    <span>{LABELS.autoReplyDelay}</span>
                    <input
                      className="auto-reply-input"
                      value={autoReplyDelay}
                      onChange={(event) => {
                        setAutoReplyDelay(event.target.value.replace(/[^\d]/g, ''))
                        setAutoReplyTimingDirty(true)
                      }}
                    />
                  </label>
                  <label className="auto-reply-field">
                    <span>{LABELS.autoReplyScheduleInterval}</span>
                    <input
                      className="auto-reply-input"
                      value={autoReplyScheduleInterval}
                      onChange={(event) => {
                        setAutoReplyScheduleInterval(event.target.value.replace(/[^\d]/g, ''))
                        setAutoReplyTimingDirty(true)
                      }}
                    />
                  </label>
                </div>
                <div className="auto-reply-card-actions">
                  <button
                    type="button"
                    className="text-button text-button-primary"
                    onClick={saveAutoReplyTiming}
                    disabled={busyAction === 'saveAutoReplyTiming'}
                  >
                    {busyAction === 'saveAutoReplyTiming' ? LABELS.executing : LABELS.autoReplySaveTiming}
                  </button>
                </div>
              </article>

              <article className="about-card auto-reply-card auto-reply-log-card">
                <span className="about-label">{LABELS.autoReplyLogs}</span>
                <strong>{LABELS.autoReplyLogs}</strong>
                {autoReplyRecentLogs.length > 0 ? (
                  <ul className="voice-log-list auto-reply-log-list">
                    {autoReplyRecentLogs.map((line, index) => (
                      <li key={`${index}-${line}`}>{line}</li>
                    ))}
                  </ul>
                ) : (
                  <p>{LABELS.autoReplyNoLogs}</p>
                )}
              </article>
            </section>
            </section>
          )}
        </div>

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

        {page !== 'danmaku' && (
          <footer className="frame-footer">
            <div className="build-tag">{`${TEXT.versionPrefix}${buildLabel}`}</div>
          </footer>
        )}
      </main>
    </div>
  )
}
