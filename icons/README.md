# Z Cast 应用图标设计

## 设计理念

以 **"Z"** 为核心设计元素，采用简约大气的线条风格，符合现代应用设计美学。

---

## 图标文件

| 文件 | 风格 | 背景 | Z字颜色 |
|------|------|------|---------|
| `icon-gradient.svg` | 极简渐变 | 紫蓝渐变 | 白色 |
| `icon-dark.svg` | 深色极简 | 深蓝 #1a1a2e | 青蓝 #00d4ff |
| `icon-light.svg` | 白色极简 | 白色 | 紫蓝渐变 |
| `icon-rounded.svg` | 圆角 Z | 粉紫渐变 | 白色 |
| `icon-double.svg` | 双层线条 | 深色 | 青蓝双层 |
| `icon-dynamic.svg` | 动态线条 | 黑色 | 蓝色渐变 |

---

## 预览

在浏览器中打开 `icon-preview.html` 查看所有图标效果。

---

## 如何使用

### 方法一：在线转换

1. 访问 [Android Asset Studio](https://romannurik.github.io/AndroidAssetStudio/icons-launcher.html)
2. 上传 SVG 文件
3. 下载生成的图标包
4. 解压到 `app/src/main/res/` 目录

### 方法二：Android Studio

1. 右键点击 `res` 目录
2. 选择 `New` → `Image Asset`
3. 选择 SVG 文件作为源
4. 调整设置并生成

### 方法三：手动转换

使用 [svg2android](https://inloop.github.io/svg2android/) 将 SVG 转换为 Vector Drawable XML。

---

## 推荐方案

**投屏应用推荐使用：**

- **方案一 (icon-gradient.svg)** - 科技感强，紫蓝渐变符合投屏主题
- **方案二 (icon-dark.svg)** - 简约专业，青蓝色线条醒目

---

## 设计特点

- ✅ **简约线条** - 仅使用 Z 字线条，无多余装饰
- ✅ **现代配色** - 渐变或单色，符合 Material Design
- ✅ **高辨识度** - Z 字元素突出，易于识别
- ✅ **多尺寸适配** - SVG 矢量格式，任意缩放不失真
