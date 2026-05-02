from pathlib import Path

from PIL import Image, ImageDraw


REPO_ROOT = Path(__file__).resolve().parent.parent
WINDOWS_RESOURCES = REPO_ROOT / "windows-receiver" / "resources"
PREVIEW_PATH = WINDOWS_RESOURCES / "z6y-logo-preview.png"
ICO_PATH = WINDOWS_RESOURCES / "z6y-logo.ico"

BACKGROUND = (15, 23, 42, 255)
BACKGROUND_STROKE = (37, 50, 74, 255)
HIGHLIGHT = (255, 255, 255, 15)
SHADOW = (25, 48, 76, 145)
MAIN_Z = (247, 251, 255, 255)
ACCENT_GLOW = (105, 185, 255, 31)
ACCENT_DOT = (105, 185, 255, 255)


def rounded_rectangle(draw: ImageDraw.ImageDraw, box: tuple[int, int, int, int], radius: int, fill, outline=None, width: int = 1):
    draw.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def render_logo(size: int) -> Image.Image:
    image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    scale = size / 512.0

    def px(value: float) -> int:
        return int(round(value * scale))

    rounded_rectangle(
        draw,
        (px(52), px(52), px(460), px(460)),
        px(112),
        fill=BACKGROUND,
    )
    rounded_rectangle(
        draw,
        (px(54), px(54), px(458), px(458)),
        px(110),
        fill=None,
        outline=BACKGROUND_STROKE,
        width=max(1, px(4)),
    )

    draw.ellipse(
        (px(58), px(80), px(266), px(204)),
        fill=HIGHLIGHT,
    )

    shadow_points = [
        (160, 172),
        (370, 172),
        (188, 354),
        (380, 354),
    ]
    main_points = [
        (150, 160),
        (360, 160),
        (178, 342),
        (370, 342),
    ]

    draw.line(
        [(px(x), px(y)) for x, y in shadow_points],
        fill=SHADOW,
        width=max(4, px(54)),
        joint="curve",
    )
    draw.line(
        [(px(x), px(y)) for x, y in main_points],
        fill=MAIN_Z,
        width=max(4, px(46)),
        joint="curve",
    )

    glow_radius = px(30)
    glow_center = (px(384), px(348))
    draw.ellipse(
        (
            glow_center[0] - glow_radius,
            glow_center[1] - glow_radius,
            glow_center[0] + glow_radius,
            glow_center[1] + glow_radius,
        ),
        fill=ACCENT_GLOW,
    )

    dot_radius = px(18)
    draw.ellipse(
        (
            glow_center[0] - dot_radius,
            glow_center[1] - dot_radius,
            glow_center[0] + dot_radius,
            glow_center[1] + dot_radius,
        ),
        fill=ACCENT_DOT,
    )

    return image


def main() -> None:
    WINDOWS_RESOURCES.mkdir(parents=True, exist_ok=True)

    preview = render_logo(512)
    preview.save(PREVIEW_PATH)

    sizes = [16, 24, 32, 40, 48, 64, 128, 256]
    frames = [render_logo(size) for size in sizes]
    frames[0].save(
        ICO_PATH,
        format="ICO",
        sizes=[(size, size) for size in sizes],
        append_images=frames[1:],
    )

    print(f"Preview written to: {PREVIEW_PATH}")
    print(f"Icon written to: {ICO_PATH}")


if __name__ == "__main__":
    main()
