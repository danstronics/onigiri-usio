"""Generate the controller manual diagrams in manual/.

Usage (from the repository root):
    1. build the host tests and run ui-test in a scratch folder: it writes
       manual-XX.pbm screen captures (tests/ui/ui_preview.c);
    2. python tools/gen_manual.py <folder with manual-*.pbm>

Each diagram is manual/onigiri.svg (ITAIKO-Web) with the function of every
button written on it and the real OLED screen of that game inside the display.
Needs Pillow.
"""
import base64
import io
import re
import sys
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
MANUAL = ROOT / 'manual'
FONT = 'Arial, Helvetica, sans-serif'

# Button centres in onigiri.svg coordinates (after each group's transform).
TOP = {'l': (202.2, 434.6), 'minus': (271.8, 475.9), 'share': (339.6, 518.9),
       'home': (670.4, 517.0), 'plus': (740.6, 476.7), 'r': (811.4, 438.8)}
FACE = {'north': (800.5, 611.4), 'west': (714.2, 695.9), 'east': (885.6, 695.9), 'south': (800.5, 782.2)}
DPAD_LABEL = (215.4, 836)
SCREEN = (391.571, 673.874, 231.426, 122.361)
# The two small buttons in the top notch: BOOT (BOOTSEL) left, RESET right.
BOOT = (445.514, 280.52, 40.269, 51.775)
RESET = (527.5, 280.52, 40.269, 51.775)

SYSTEM = {'minus': 'SERVICE', 'share': 'COIN', 'home': 'TEST'}
PAGES = [
    ('game-select', 'Game Select', 'menu', {
        'minus': 'BACK', 'plus': 'OK', 'dpad': 'UP / DOWN: CHOOSE'}),
    ('tekken-6', 'Tekken 6 · Tekken 6 BR', '04', {
        **SYSTEM, 'plus': 'START', 'dpad': 'MOVE',
        'west': 'LP', 'north': 'RP', 'south': 'LK', 'east': 'RK'}),
    ('tekken-tag-2', 'Tekken Tag 2 · Unlimited', '01', {
        **SYSTEM, 'plus': 'START', 'r': 'BTN 5', 'dpad': 'MOVE',
        'west': 'LP', 'north': 'RP', 'south': 'LK', 'east': 'RK'}),
    ('fighters', 'Dragon Ball Zenkai · Gundam Maxi Boost', '09', {
        **SYSTEM, 'plus': 'START', 'r': 'BTN 5', 'dpad': 'MOVE',
        'west': 'BTN 1', 'north': 'BTN 2', 'south': 'BTN 3', 'east': 'BTN 4'}),
    ('shooters', 'Razing Storm · Dark Escape 4D · Sailor Zombie', '02', {
        **SYSTEM, 'plus': 'ENTER', 'dpad': 'AIM',
        'west': 'START', 'north': 'EXTRA', 'south': 'FIRE', 'east': 'FIRE 2'}),
    ('deadstorm', 'Deadstorm Pirates', '03', {
        **SYSTEM, 'plus': 'ENTER', 'l': 'DX', 'dpad': 'AIM',
        'west': 'START', 'north': 'EXTRA', 'south': 'FIRE', 'east': 'FIRE 2'}),
    ('taiko', 'Taiko no Tatsujin', '00', {
        **SYSTEM, 'plus': 'ENTER', 'dpad': 'UP / DOWN: MENU',
        'west': 'KA L', 'north': 'DON L', 'south': 'DON R', 'east': 'KA R'}),
    ('update', 'Firmware update', None, {}),
]


def controller_body():
    text = (MANUAL / 'onigiri.svg').read_text(encoding='utf-8')
    return text[text.index('>', text.index('<svg')) + 1:text.rindex('</svg>')]


def screen_image(folder, key):
    image = Image.open(Path(folder) / f'manual-{key}.pbm').convert('L').point(lambda v: 255 - v)
    buffer = io.BytesIO(); image.save(buffer, 'PNG')
    x, y, w, h = SCREEN
    scale = min(w / 128, h / 64)
    iw, ih = 128 * scale, 64 * scale
    data = base64.b64encode(buffer.getvalue()).decode()
    return (f'<image x="{x + (w - iw) / 2:.1f}" y="{y + (h - ih) / 2:.1f}" width="{iw:.1f}" height="{ih:.1f}" '
            f'style="image-rendering:pixelated" href="data:image/png;base64,{data}" '
            f'xlink:href="data:image/png;base64,{data}"/>')


def text(x, y, label, size, weight='bold', fill='black'):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-family="{FONT}" font-size="{size}" font-weight="{weight}" '
            f'fill="{fill}" text-anchor="middle" dominant-baseline="middle">{label}</text>')


def small_button(rect, fill, label, step, anchor):
    x, y, w, h = rect
    tx = x - 18 if anchor == 'end' else x + w + 18
    return [f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" fill="{fill}" stroke="black" stroke-width="4.08"/>',
            f'<text x="{tx:.1f}" y="{y + 14:.1f}" font-family="{FONT}" font-size="26" font-weight="bold" '
            f'fill="{fill}" text-anchor="{anchor}" dominant-baseline="middle">{label}</text>',
            f'<text x="{tx:.1f}" y="{y + 42:.1f}" font-family="{FONT}" font-size="17" font-weight="bold" '
            f'fill="#333" text-anchor="{anchor}" dominant-baseline="middle">{step}</text>']


def update_steps():
    parts = small_button(BOOT, '#d9480f', 'BOOT', '1. HOLD', 'end')
    parts += small_button(RESET, '#1971c2', 'RESET', '2. PRESS ONCE', 'start')
    parts.append(text(507, 400, '3. RELEASE BOOT', 20, 'bold', '#d9480f'))
    parts.append(text(507, 430, 'RPI-RP2 DRIVE APPEARS', 17, 'bold', '#555'))
    parts.append(text(507, 456, 'COPY THE .UF2 ONTO IT', 17, 'bold', '#555'))
    return parts


def diagram(title, screen, labels, folder):
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" xmlns:serif="http://www.serif.com/" xmlns:xlink="http://www.w3.org/1999/xlink" viewBox="0 -10 1024 960" '
             'width="1024" height="960" style="fill-rule:evenodd;clip-rule:evenodd;stroke-linecap:round;stroke-linejoin:round;">',
             '<rect x="0" y="-10" width="1024" height="960" fill="white"/>',
             text(512, 30, title, 34),
             controller_body()]
    if screen is None:
        parts += update_steps()
        parts.append('</svg>')
        return '\n'.join(parts)
    parts.append(screen_image(folder, screen))
    for key, (x, y) in TOP.items():
        if key in labels:   # Nudged towards the middle, clear of the neighbour button.
            parts.append(text(x + (8 if x < 512 else -8), y - 42, labels[key], 18))
    for key, (x, y) in FACE.items():
        if key in labels:
            size = 19 if len(labels[key]) <= 5 else 16
            parts.append(text(x, y + 1, labels[key], size))
    if 'dpad' in labels:
        parts.append(text(*DPAD_LABEL, labels['dpad'], 22))
    if screen != 'menu':
        parts.append(text(505, 432, 'HOLD  −  +  2 s', 17, 'bold', '#555'))
        parts.append(text(505, 456, 'GAME SELECT', 17, 'bold', '#555'))
    else:
        parts.append(text(505, 432, 'HOLD  −  +  2 s', 22))
        parts.append(text(505, 460, 'TO OPEN', 18, 'bold', '#555'))
    parts.append('</svg>')
    return '\n'.join(parts)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    for name, title, screen, labels in PAGES:
        out = MANUAL / f'{name}.svg'
        out.write_text(diagram(title, screen, labels, sys.argv[1]), encoding='utf-8')
        print('written', out.relative_to(ROOT))
